#include "game_ui_virtbag.h"

#include "game_access.h"
#include "game_ops_common.h"
#include "game_state.h"
#include "game_symbols.h"
#include "stack_codec.h"
#include "virtual_bag_state.h"

#include <android/log.h>

#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <sys/mman.h>
#include <thread>
#include <unistd.h>

#define VIRTBAG_TAG "Inotia4VirtBag"
#define VIRTBAG_LOG(...) __android_log_print(ANDROID_LOG_INFO, VIRTBAG_TAG, __VA_ARGS__)

namespace {

constexpr size_t kPopupStateSize = 0x40;
constexpr int kPopupStateCount = 27;
// 触摸事件使用 Scene_Draw 的绝对逻辑坐标；真机运行时宽度为 1408。
constexpr int64_t kCellX = 0x4c4;
constexpr int64_t kCellY = 0x86;
constexpr int64_t kCellWidth = 0x39;
constexpr int64_t kCellHeight = 0x39;
constexpr int64_t kCellStepY = 0x46;
constexpr size_t kInventorySlotStride = 16;
constexpr uint8_t kNoOriginalBagSelected = 6;

using PopupEventFn = uint64_t (*)(uint64_t, uint64_t, uint64_t);
using PopupNoArgFn = void (*)();
using OriginalDrawInvenBagFn = void (*)();

std::mutex g_virtual_bag_mtx;
std::atomic<bool> g_inject_thread_started{false};
std::atomic<uint64_t> g_exit_trace_sequence{0};
jclass g_virtual_bag_bridge_class = nullptr;
uint8_t* g_state_entry = nullptr;
PopupEventFn g_orig_event = nullptr;
PopupNoArgFn g_orig_f3 = nullptr;
uintptr_t g_draw_patch_addr = 0;
uintptr_t g_bag_draw_patch_addr = 0;
void* g_draw_thunk = nullptr;
void* g_bag_draw_thunk = nullptr;
virtual_bag::State g_virtual_bag_state{};
int g_loaded_slot = -2;
std::array<std::array<void*, virtual_bag::kSlotCount>, virtual_bag::kBagCount> g_module_objects{};
std::array<std::array<int, virtual_bag::kSlotCount>, virtual_bag::kBagCount> g_module_object_categories{};
std::array<void*, 96> g_original_inventory{};
uint8_t g_original_current_direct = 0;
uint8_t g_original_current_got = 0;
uint32_t* g_original_bag_size_word = nullptr;
uint32_t g_original_bag_size = 0;
bool g_module_view_installed = false;
int g_module_view_index = -1;

int font_id() {
    if (g_base == 0) return 1;
    void** slot = reinterpret_cast<void**>(g_base + G_FONT_OBJ_SLOT_VMA);
    if (slot == nullptr || *slot == nullptr) return 1;
    return *reinterpret_cast<int*>(reinterpret_cast<uint8_t*>(*slot) + 4);
}

int raw_direct_bag_locked() {
    if (g_base == 0) return -1;
    return *reinterpret_cast<uint8_t*>(g_base + G_UIEQUIP_CUR_BAG_VMA);
}

int raw_got_bag_locked() {
    if (g_base == 0) return -1;
    uint8_t** current_bag = reinterpret_cast<uint8_t**>(g_base + G_UIEQUIP_CUR_BAG_GOT_VMA);
    return current_bag != nullptr && *current_bag != nullptr ? **current_bag : -1;
}

int raw_desc_type_locked() {
    return g_base == 0 ? -1 : *reinterpret_cast<uint8_t*>(g_base + G_UIEQUIP_DESC_TYPE_VMA);
}

void log_exit_trace_locked(const char* phase, uint64_t event, uint64_t param, uint64_t param2,
                           int64_t x = -1, int64_t y = -1) {
    const uint64_t sequence = g_exit_trace_sequence.fetch_add(1) + 1;
    const auto now = std::chrono::steady_clock::now().time_since_epoch();
    const auto milliseconds = std::chrono::duration_cast<std::chrono::milliseconds>(now).count();
    VIRTBAG_LOG("exit seq=%llu ms=%lld phase=%s event=0x%llx param=0x%llx param2=0x%llx x=%lld y=%lld mode=%d selected=%d inspected=%d overlay=%d overlay_index=%d direct=%d got=%d desc=%d saved_direct=%u saved_got=%u",
                static_cast<unsigned long long>(sequence), static_cast<long long>(milliseconds), phase,
                static_cast<unsigned long long>(event), static_cast<unsigned long long>(param),
                static_cast<unsigned long long>(param2), static_cast<long long>(x), static_cast<long long>(y),
                static_cast<int>(g_virtual_bag_state.mode), g_virtual_bag_state.selected,
                g_virtual_bag_state.inspected, g_module_view_installed ? 1 : 0, g_module_view_index,
                raw_direct_bag_locked(), raw_got_bag_locked(), raw_desc_type_locked(),
                static_cast<unsigned int>(g_original_current_direct),
                static_cast<unsigned int>(g_original_current_got));
}

uint8_t* find_inventory_state_entry() {
    if (g_base == 0) return nullptr;
    void** got = reinterpret_cast<void**>(g_base + G_POPUP_STATE_LIST_GOT_VMA);
    if (got == nullptr || *got == nullptr) return nullptr;
    uint8_t* list = reinterpret_cast<uint8_t*>(*got);
    for (int index = 0; index < kPopupStateCount; ++index) {
        uint8_t* entry = list + index * kPopupStateSize;
        uintptr_t enter = *reinterpret_cast<uintptr_t*>(entry + 0x10);
        if (enter == g_base + fn_resolve("F_PANEL_INVENTORY_ENTER", F_PANEL_INVENTORY_ENTER)) {
            return entry;
        }
    }
    return nullptr;
}

bool parse_int(const char* text, const char** cursor, int* value) {
    if (text == nullptr || cursor == nullptr || *cursor == nullptr || value == nullptr) return false;
    char* end = nullptr;
    long parsed = strtol(*cursor, &end, 10);
    if (end == *cursor || parsed < -1 || parsed > virtual_bag::kMaxCapacity) return false;
    *value = static_cast<int>(parsed);
    *cursor = end;
    return true;
}

bool parse_state_json(const char* json, virtual_bag::State* state) {
    if (json == nullptr || state == nullptr) return false;
    const char* types = strstr(json, "\"types\":[");
    if (types == nullptr) return false;
    const char* cursor = types + strlen("\"types\":[");
    virtual_bag::State parsed{};
    for (int index = 0; index < virtual_bag::kBagCount; ++index) {
        int type = 0;
        if (!parse_int(json, &cursor, &type) || !virtual_bag::valid_type(type)) return false;
        parsed.types[index] = static_cast<uint8_t>(type);
        if (index + 1 < virtual_bag::kBagCount) {
            if (*cursor != ',') return false;
            ++cursor;
        }
    }
    if (*cursor != ']') return false;
    const char* mode = strstr(json, "\"mode\":\"module\"");
    parsed.mode = mode == nullptr ? virtual_bag::Mode::kOriginal : virtual_bag::Mode::kModule;
    const char* original_selected = strstr(json, "\"originalSelected\":");
    const char* selected = strstr(json, "\"selected\":");
    const char* inspected = strstr(json, "\"inspected\":");
    if (original_selected == nullptr || selected == nullptr || inspected == nullptr) return false;
    cursor = original_selected + strlen("\"originalSelected\":");
    char* end = nullptr;
    long original = strtol(cursor, &end, 10);
    if (end == cursor || original < 0 || original >= 6) return false;
    parsed.original_selected = static_cast<int>(original);
    cursor = selected + strlen("\"selected\":");
    if (!parse_int(json, &cursor, &parsed.selected)) return false;
    cursor = inspected + strlen("\"inspected\":");
    if (!parse_int(json, &cursor, &parsed.inspected)) return false;
    const char* items = strstr(json, "\"items\":[");
    if (items == nullptr) return false;
    cursor = items + strlen("\"items\":[");
    for (int bag = 0; bag < virtual_bag::kBagCount; ++bag) {
        for (int slot = 0; slot < virtual_bag::kSlotCount; ++slot) {
            const char* category = strstr(cursor, "\"category\":");
            const char* count = strstr(cursor, "\"count\":");
            if (category == nullptr || count == nullptr) return false;
            char* category_end = nullptr;
            long category_value = strtol(category + strlen("\"category\":"), &category_end, 10);
            char* count_end = nullptr;
            long count_value = strtol(count + strlen("\"count\":"), &count_end, 10);
            if (category_end == category + strlen("\"category\":") ||
                count_end == count + strlen("\"count\":") || category_value < 0 || count_value < 0) {
                return false;
            }
            parsed.items[bag][slot] = {static_cast<int>(category_value), static_cast<int>(count_value)};
            cursor = count_end;
        }
    }
    virtual_bag::normalize(&parsed);
    *state = parsed;
    return true;
}

std::string state_json(const virtual_bag::State& state) {
    std::string json = "{\"mode\":\"";
    json += state.mode == virtual_bag::Mode::kModule ? "module" : "original";
    json += "\",\"originalSelected\":" + std::to_string(state.original_selected);
    json += ",\"types\":[";
    for (int index = 0; index < virtual_bag::kBagCount; ++index) {
        if (index > 0) json += ',';
        json += std::to_string(state.types[index]);
    }
    json += "],\"capacities\":[";
    for (int index = 0; index < virtual_bag::kBagCount; ++index) {
        if (index > 0) json += ',';
        json += std::to_string(state.capacities[index]);
    }
    json += "],\"selected\":" + std::to_string(state.selected);
    json += ",\"inspected\":" + std::to_string(state.inspected) + ",\"items\":[";
    for (int bag = 0; bag < virtual_bag::kBagCount; ++bag) {
        if (bag > 0) json += ',';
        json += '[';
        for (int slot = 0; slot < virtual_bag::kSlotCount; ++slot) {
            if (slot > 0) json += ',';
            const virtual_bag::Item& item = state.items[bag][slot];
            json += "{\"category\":" + std::to_string(item.category) +
                    ",\"count\":" + std::to_string(item.count) + "}";
        }
        json += ']';
    }
    json += "]}";
    return json;
}

JNIEnv* current_env() {
    JavaVM* jvm = g_jvm();
    if (jvm == nullptr) return nullptr;
    JNIEnv* env = nullptr;
    if (jvm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6) == JNI_OK) return env;
    return jvm->AttachCurrentThread(&env, nullptr) == JNI_OK ? env : nullptr;
}

bool load_state_from_store(int slot) {
    JNIEnv* env = current_env();
    if (env == nullptr || g_virtual_bag_bridge_class == nullptr) return false;
    jmethodID method = env->GetStaticMethodID(g_virtual_bag_bridge_class, "loadStateJson", "(I)Ljava/lang/String;");
    if (method == nullptr) {
        env->ExceptionClear();
        return false;
    }
    jstring result = static_cast<jstring>(env->CallStaticObjectMethod(g_virtual_bag_bridge_class, method, slot));
    if (env->ExceptionCheck()) {
        env->ExceptionClear();
        return false;
    }
    if (result == nullptr) return false;
    const char* utf = env->GetStringUTFChars(result, nullptr);
    const bool parsed = utf != nullptr && parse_state_json(utf, &g_virtual_bag_state);
    if (utf != nullptr) env->ReleaseStringUTFChars(result, utf);
    env->DeleteLocalRef(result);
    return parsed;
}

bool save_state_to_store(int slot) {
    JNIEnv* env = current_env();
    if (env == nullptr || g_virtual_bag_bridge_class == nullptr) return false;
    jmethodID method = env->GetStaticMethodID(g_virtual_bag_bridge_class, "saveStateJson",
                                               "(ILjava/lang/String;)Ljava/lang/String;");
    if (method == nullptr) {
        env->ExceptionClear();
        return false;
    }
    const std::string json = state_json(g_virtual_bag_state);
    jstring payload = env->NewStringUTF(json.c_str());
    if (payload == nullptr) return false;
    jstring result = static_cast<jstring>(env->CallStaticObjectMethod(g_virtual_bag_bridge_class, method, slot, payload));
    env->DeleteLocalRef(payload);
    if (env->ExceptionCheck()) {
        env->ExceptionClear();
        return false;
    }
    if (result == nullptr) return false;
    const char* utf = env->GetStringUTFChars(result, nullptr);
    const bool ok = utf != nullptr && strcmp(utf, "ok") == 0;
    if (utf != nullptr) env->ReleaseStringUTFChars(result, utf);
    env->DeleteLocalRef(result);
    return ok;
}

void ensure_state_loaded_locked() {
    const int slot = current_save_slot();
    if (slot < 0 || slot > 2 || slot == g_loaded_slot) return;
    g_virtual_bag_state = {};
    if (!load_state_from_store(slot)) {
        VIRTBAG_LOG("virtual bag state slot=%d unavailable; using empty state", slot);
    }
    g_loaded_slot = slot;
}

bool persist_state_locked() {
    const int slot = current_save_slot();
    if (slot < 0 || slot > 2) return false;
    if (!save_state_to_store(slot)) {
        VIRTBAG_LOG("virtual bag state slot=%d save failed", slot);
        return false;
    }
    g_loaded_slot = slot;
    return true;
}

bool cell_hit(int index, int64_t x, int64_t y) {
    if (!virtual_bag::valid_index(index)) return false;
    const int64_t cell_y = kCellY + index * kCellStepY;
    return x >= kCellX && x < kCellX + kCellWidth && y >= cell_y && y < cell_y + kCellHeight;
}

void draw_cells_in_frame_locked() {
    const bool can_draw_original_button = fn_grpx_draw_part != nullptr && fn_imgsys_get_group != nullptr &&
                                          fn_imgsys_get_loc != nullptr;
    if (!can_draw_original_button) return;
    void* group = can_draw_original_button ? fn_imgsys_get_group(0xf) : nullptr;
    for (int index = 0; index < virtual_bag::kBagCount; ++index) {
        const int64_t y = kCellY + index * kCellStepY;
        constexpr int dark_icon_loc = 0x9;
        if (can_draw_original_button && group != nullptr) {
            void* loc = fn_imgsys_get_loc(0xf, dark_icon_loc);
            fn_grpx_draw_part(group, static_cast<int32_t>(kCellX + 5), static_cast<int32_t>(y + 5),
                              loc, 0, 1, 0);
        }
    }
}

int original_bag_locked();

uint32_t* original_bag_size_word_locked() {
    if (g_base == 0) return nullptr;
    void*** table_slot = reinterpret_cast<void***>(g_base + G_BAG_TABLE_VMA);
    if (table_slot == nullptr || *table_slot == nullptr) return nullptr;
    const int bag = original_bag_locked();
    if (bag < 0 || bag >= 6) return nullptr;
    void* bag_object = (*table_slot)[bag];
    if (bag_object == nullptr) return nullptr;
    return reinterpret_cast<uint32_t*>(static_cast<uint8_t*>(bag_object) + 0x10);
}

void refresh_module_item_area_locked(int index) {
    if (fn_ui_equip_refresh_item_area == nullptr) return;
    uint32_t* size_word = g_original_bag_size_word;
    if (size_word == nullptr) {
        fn_ui_equip_refresh_item_area();
        return;
    }
    constexpr uint32_t kCapacityMask = (1u << 25) - 1u;
    *size_word = (g_original_bag_size & ~kCapacityMask) |
                 (static_cast<uint32_t>(g_virtual_bag_state.capacities[index]) & kCapacityMask);
    fn_ui_equip_refresh_item_area();
}

void virtual_bag_draw_original_bag_wrapper() {
    const OriginalDrawInvenBagFn original =
        reinterpret_cast<OriginalDrawInvenBagFn>(g_base + fn_resolve("F_UIEQUIP_DRAW_INVEN_BAG_VMA",
                                                                     F_UIEQUIP_DRAW_INVEN_BAG_VMA));
    if (original == nullptr) return;
    std::lock_guard<std::mutex> lock(g_virtual_bag_mtx);
    ensure_state_loaded_locked();
    const bool module_view = g_virtual_bag_state.mode == virtual_bag::Mode::kModule &&
                             g_module_view_installed;
    static int last_module_view = -1;
    if (last_module_view != static_cast<int>(module_view)) {
        log_exit_trace_locked("draw_bag", 0, 0, 0);
        last_module_view = module_view ? 1 : 0;
    }
    uint8_t** current_bag = reinterpret_cast<uint8_t**>(g_base + G_UIEQUIP_CUR_BAG_GOT_VMA);
    uint8_t saved_current = 0;
    bool masked = false;
    if (module_view && current_bag != nullptr && *current_bag != nullptr) {
        saved_current = **current_bag;
        **current_bag = kNoOriginalBagSelected;
        masked = true;
    }
    original();
    if (masked) **current_bag = saved_current;
}

int original_bag_locked() {
    if (g_base == 0) return 0;
    uint8_t** current_bag = reinterpret_cast<uint8_t**>(g_base + G_UIEQUIP_CUR_BAG_GOT_VMA);
    if (current_bag != nullptr && *current_bag != nullptr && **current_bag < 6) {
        return **current_bag;
    }
    const uint8_t direct = *reinterpret_cast<uint8_t*>(g_base + G_UIEQUIP_CUR_BAG_VMA);
    if (direct < 6) return direct;
    return 0;
}

void set_original_bag_locked(int bag) {
    if (g_base == 0 || bag < 0 || bag >= 6) return;
    *reinterpret_cast<uint8_t*>(g_base + G_UIEQUIP_CUR_BAG_VMA) = static_cast<uint8_t>(bag);
    uint8_t** current_bag = reinterpret_cast<uint8_t**>(g_base + G_UIEQUIP_CUR_BAG_GOT_VMA);
    if (current_bag != nullptr && *current_bag != nullptr) **current_bag = static_cast<uint8_t>(bag);
    log_exit_trace_locked("write_set", 0, 0, 0);
}

bool original_got_bag_locked(int* bag) {
    if (bag == nullptr || g_base == 0) return false;
    uint8_t** current_bag = reinterpret_cast<uint8_t**>(g_base + G_UIEQUIP_CUR_BAG_GOT_VMA);
    if (current_bag == nullptr || *current_bag == nullptr || **current_bag >= kNoOriginalBagSelected) {
        return false;
    }
    *bag = **current_bag;
    return true;
}

void clear_original_bag_selection_locked() {
    if (g_base == 0) return;
    *reinterpret_cast<uint8_t*>(g_base + G_UIEQUIP_CUR_BAG_VMA) = kNoOriginalBagSelected;
    uint8_t** current_bag = reinterpret_cast<uint8_t**>(g_base + G_UIEQUIP_CUR_BAG_GOT_VMA);
    if (current_bag != nullptr && *current_bag != nullptr) **current_bag = kNoOriginalBagSelected;
    log_exit_trace_locked("write_clear", 0, 0, 0);
}

void clear_original_desc_locked() {
    if (g_base == 0) return;
    *reinterpret_cast<uint8_t*>(g_base + G_UIEQUIP_DESC_TYPE_VMA) = 0;
    if (fn_ui_desc_set_off != nullptr) fn_ui_desc_set_off();
}

bool original_bag_button_hit(int64_t x, int64_t y) {
    // UIEquip original bag controls are children of nested parents. The final absolute
    // rect for bag 0 is (1116, 145, 57, 57); each subsequent original bag is 70px lower.
    constexpr int64_t kOriginalBagX = 0x45c;
    constexpr int64_t kOriginalBagWidth = 0x39;
    constexpr int64_t kOriginalBagY = 0x91;
    constexpr int64_t kOriginalBagStepY = 0x46;
    return x >= kOriginalBagX && x < kOriginalBagX + kOriginalBagWidth &&
           y >= kOriginalBagY && y < kOriginalBagY + kOriginalBagStepY * 6;
}

void* module_item_locked(int bag, int slot) {
    if (!virtual_bag::valid_index(bag) || slot < 0 || slot >= virtual_bag::kSlotCount) return nullptr;
    const virtual_bag::Item& descriptor = g_virtual_bag_state.items[bag][slot];
    if (descriptor.category <= 0 || descriptor.count <= 0 || fn_create_item == nullptr) return nullptr;
    void* item = g_module_objects[bag][slot];
    if (item == nullptr || g_module_object_categories[bag][slot] != descriptor.category) {
        item = fn_create_item(descriptor.category, 0, 0, 0);
        if (item == nullptr) return nullptr;
        g_module_objects[bag][slot] = item;
        g_module_object_categories[bag][slot] = descriptor.category;
    }
    uint32_t count_flags = *reinterpret_cast<uint32_t*>(reinterpret_cast<uint8_t*>(item) + I_COUNT);
    *reinterpret_cast<uint32_t*>(reinterpret_cast<uint8_t*>(item) + I_COUNT) =
        stack_codec::write_count(count_flags, static_cast<uint32_t>(descriptor.count));
    return item;
}

void restore_module_view_locked() {
    if (!g_module_view_installed || g_inven == nullptr || g_base == 0) return;
    void** inventory = static_cast<void**>(g_inven);
    for (size_t slot = 0; slot < g_original_inventory.size(); ++slot) inventory[slot] = g_original_inventory[slot];
    if (g_original_bag_size_word != nullptr) *g_original_bag_size_word = g_original_bag_size;
    *reinterpret_cast<uint8_t*>(g_base + G_UIEQUIP_CUR_BAG_VMA) = g_original_current_direct;
    uint8_t** current_bag = reinterpret_cast<uint8_t**>(g_base + G_UIEQUIP_CUR_BAG_GOT_VMA);
    if (current_bag != nullptr && *current_bag != nullptr) **current_bag = g_original_current_got;
    if (fn_ui_equip_refresh_item_area != nullptr) fn_ui_equip_refresh_item_area();
    g_original_bag_size_word = nullptr;
    g_original_bag_size = 0;
    g_module_view_installed = false;
    g_module_view_index = -1;
    log_exit_trace_locked("write_restore", 0, 0, 0);
}

bool complete_original_exit_locked() {
    int selected_original_bag = 0;
    if (!original_got_bag_locked(&selected_original_bag)) {
        log_exit_trace_locked("complete_invalid_got", 0, 0, 0);
        return false;
    }
    set_original_bag_locked(selected_original_bag);
    if (fn_ui_equip_refresh_item_area != nullptr) fn_ui_equip_refresh_item_area();
    virtual_bag::enter_original(&g_virtual_bag_state, selected_original_bag);
    persist_state_locked();
    log_exit_trace_locked("complete_success", 0, 0, 0);
    return true;
}

void cancel_original_exit_locked() {
    const int original_bag = g_original_current_got < kNoOriginalBagSelected ?
                             g_original_current_got : g_original_current_direct;
    set_original_bag_locked(original_bag);
    if (fn_ui_equip_refresh_item_area != nullptr) fn_ui_equip_refresh_item_area();
    virtual_bag::enter_original(&g_virtual_bag_state, original_bag);
    persist_state_locked();
    log_exit_trace_locked("complete_cancel", 0, 0, 0);
}

void virtual_bag_f3_wrapper() {
    PopupNoArgFn original = nullptr;
    {
        std::lock_guard<std::mutex> lock(g_virtual_bag_mtx);
        ensure_state_loaded_locked();
        if (g_module_view_installed) {
            restore_module_view_locked();
            virtual_bag::enter_original(&g_virtual_bag_state, original_bag_locked());
            persist_state_locked();
        } else if (g_virtual_bag_state.mode == virtual_bag::Mode::kExitingModule) {
            cancel_original_exit_locked();
        }
        log_exit_trace_locked("f3", 0, 0, 0);
        original = g_orig_f3;
    }
    if (original != nullptr) original();
}

bool install_module_view_locked(int index) {
    if (!virtual_bag::valid_index(index) || g_base == 0 || g_inven == nullptr) return false;
    if (g_virtual_bag_state.capacities[index] == 0) {
        restore_module_view_locked();
        return true;
    }
    if (g_module_view_installed && g_module_view_index == index) {
        void** inventory = static_cast<void**>(g_inven);
        const int capacity = g_virtual_bag_state.capacities[index];
        const size_t bag_offset = static_cast<size_t>(g_original_current_direct) * kInventorySlotStride;
        for (int slot = 0; slot < capacity; ++slot) {
            inventory[bag_offset + slot] = module_item_locked(index, slot);
        }
        for (int slot = capacity; slot < virtual_bag::kSlotCount; ++slot) {
            inventory[bag_offset + slot] = nullptr;
        }
        refresh_module_item_area_locked(index);
        return true;
    }
    if (g_module_view_installed) restore_module_view_locked();
    void** inventory = static_cast<void**>(g_inven);
    for (size_t slot = 0; slot < g_original_inventory.size(); ++slot) g_original_inventory[slot] = inventory[slot];
    const int original_bag = original_bag_locked();
    set_original_bag_locked(original_bag);
    g_original_current_direct = static_cast<uint8_t>(original_bag);
    g_original_current_got = static_cast<uint8_t>(original_bag);
    g_original_bag_size_word = original_bag_size_word_locked();
    if (g_original_bag_size_word == nullptr) return false;
    g_original_bag_size = *g_original_bag_size_word;
    const int capacity = g_virtual_bag_state.capacities[index];
    const size_t bag_offset = static_cast<size_t>(g_original_current_direct) * kInventorySlotStride;
    for (int slot = 0; slot < capacity; ++slot) {
        inventory[bag_offset + slot] = module_item_locked(index, slot);
    }
    for (int slot = capacity; slot < virtual_bag::kSlotCount; ++slot) {
        inventory[bag_offset + slot] = nullptr;
    }
    clear_original_desc_locked();
    refresh_module_item_area_locked(index);
    g_module_view_installed = true;
    g_module_view_index = index;
    return true;
}

void virtual_bag_draw_end_wrapper() {
    std::lock_guard<std::mutex> lock(g_virtual_bag_mtx);
    ensure_state_loaded_locked();
    if (g_virtual_bag_state.mode == virtual_bag::Mode::kModule &&
        virtual_bag::valid_index(g_virtual_bag_state.selected)) {
        install_module_view_locked(g_virtual_bag_state.selected);
    } else {
        restore_module_view_locked();
    }
    static int last_mode = -1;
    static int last_selected = -2;
    static int last_overlay = -1;
    static int last_overlay_index = -2;
    if (last_mode != static_cast<int>(g_virtual_bag_state.mode) ||
        last_selected != g_virtual_bag_state.selected ||
        last_overlay != static_cast<int>(g_module_view_installed) ||
        last_overlay_index != g_module_view_index) {
        log_exit_trace_locked("draw_end", 0, 0, 0);
        last_mode = static_cast<int>(g_virtual_bag_state.mode);
        last_selected = g_virtual_bag_state.selected;
        last_overlay = g_module_view_installed ? 1 : 0;
        last_overlay_index = g_module_view_index;
    }
    draw_cells_in_frame_locked();
    if (fn_grpx_end != nullptr) fn_grpx_end();
}

void* allocate_draw_thunk(uintptr_t call_addr, uintptr_t wrapper) {
#ifndef MAP_FIXED_NOREPLACE
    (void)call_addr;
    (void)wrapper;
    return nullptr;
#else
    constexpr size_t kPageSize = 4096;
    constexpr int64_t kStep = 0x00010000;
    const uintptr_t base = call_addr & ~(static_cast<uintptr_t>(kPageSize) - 1);
    for (int64_t distance = kStep; distance < 0x08000000; distance += kStep) {
        for (int sign : {1, -1}) {
            const int64_t candidate_signed = static_cast<int64_t>(base) + sign * distance;
            if (candidate_signed <= 0) continue;
            void* region = mmap(reinterpret_cast<void*>(static_cast<uintptr_t>(candidate_signed)), kPageSize,
                                PROT_READ | PROT_WRITE | PROT_EXEC,
                                MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
            if (region == MAP_FAILED) continue;

            uint32_t code[] = {
                0xa9bf7bf0, // stp x16, x30, [sp, #-16]!
                0x58000090, // ldr x16, #16 (literal at thunk+20)
                0xd63f0200, // blr x16
                0xa8c17bf0, // ldp x16, x30, [sp], #16
                0xd65f03c0, // ret
            };
            memcpy(region, code, sizeof(code));
            *reinterpret_cast<uintptr_t*>(reinterpret_cast<uint8_t*>(region) + 20) = wrapper;
            __builtin___clear_cache(reinterpret_cast<char*>(region),
                                     reinterpret_cast<char*>(reinterpret_cast<uint8_t*>(region) + 28));
            return region;
        }
    }
    return nullptr;
#endif
}

uintptr_t arm64_bl_target(uintptr_t call_addr, uint32_t instruction) {
    int64_t immediate = static_cast<int64_t>(instruction & 0x03ffffffu);
    if ((immediate & 0x02000000LL) != 0) immediate |= ~0x03ffffffLL;
    return static_cast<uintptr_t>(static_cast<int64_t>(call_addr) + (immediate << 2));
}

uint64_t virtual_bag_event(uint64_t event, uint64_t param, uint64_t param2) {
    int64_t x = 0;
    int64_t y = 0;
    bool exiting_to_original = false;
    if (event == 0x17 && param != 0) {
        x = *reinterpret_cast<const int64_t*>(param);
        y = *reinterpret_cast<const int64_t*>(param + 8);
        std::lock_guard<std::mutex> lock(g_virtual_bag_mtx);
        ensure_state_loaded_locked();
        log_exit_trace_locked("event_enter", event, param, param2, x, y);
        for (int index = 0; index < virtual_bag::kBagCount; ++index) {
            if (!cell_hit(index, x, y)) continue;
            VIRTBAG_LOG("virtual button hit index=%d x=%lld y=%lld", index,
                        static_cast<long long>(x), static_cast<long long>(y));
            if (virtual_bag::click(&g_virtual_bag_state, index) != virtual_bag::ClickResult::kIgnored) {
                install_module_view_locked(index);
                persist_state_locked();
            }
            log_exit_trace_locked("virtual_cell", event, param, param2, x, y);
            return 0;
        }
        if (g_virtual_bag_state.mode == virtual_bag::Mode::kModule && original_bag_button_hit(x, y)) {
            restore_module_view_locked();
            virtual_bag::begin_exit_module(&g_virtual_bag_state);
            clear_original_bag_selection_locked();
            exiting_to_original = true;
            log_exit_trace_locked("original_press_pre_orig", event, param, param2, x, y);
        }
    }
    if (event != 0x17) {
        std::lock_guard<std::mutex> lock(g_virtual_bag_mtx);
        ensure_state_loaded_locked();
        log_exit_trace_locked("event_enter", event, param, param2);
    }
    if (exiting_to_original) {
        const uint64_t original_result = g_orig_event != nullptr ? g_orig_event(event, param, param2) : 0;
        std::lock_guard<std::mutex> lock(g_virtual_bag_mtx);
        ensure_state_loaded_locked();
        log_exit_trace_locked("original_press_post_orig", event, param, param2, x, y);
        return original_result;
    }
    {
        std::lock_guard<std::mutex> lock(g_virtual_bag_mtx);
        ensure_state_loaded_locked();
        log_exit_trace_locked("delegate_pre_orig", event, param, param2, x, y);
    }
    const uint64_t result = g_orig_event != nullptr ? g_orig_event(event, param, param2) : 0;
    {
        std::lock_guard<std::mutex> lock(g_virtual_bag_mtx);
        ensure_state_loaded_locked();
        log_exit_trace_locked("delegate_post_orig", event, param, param2, x, y);
        if (g_virtual_bag_state.mode == virtual_bag::Mode::kExitingModule) {
            if (event == 0x18 && !complete_original_exit_locked()) {
                cancel_original_exit_locked();
            } else if (event == 0x19) {
                cancel_original_exit_locked();
            }
            return result;
        }
        // 物品格点击也会产生 0x17；原版袋按钮已在上方单独处理。
        // 这里仅处理原版事件确实改写当前袋的兜底退出路径。
        const bool original_bag_changed = g_virtual_bag_state.mode == virtual_bag::Mode::kModule &&
                                          original_bag_locked() != g_original_current_got;
        if (event == 0x17 && g_virtual_bag_state.mode == virtual_bag::Mode::kModule &&
            original_bag_changed) {
            const int selected_original_bag = original_bag_locked();
            restore_module_view_locked();
            set_original_bag_locked(selected_original_bag);
            if (fn_ui_equip_refresh_item_area != nullptr) fn_ui_equip_refresh_item_area();
            virtual_bag::enter_original(&g_virtual_bag_state, selected_original_bag);
            persist_state_locked();
        } else if (event == 0x17 || event == 0x18) {
            g_virtual_bag_state.original_selected = original_bag_locked();
        }
    }
    return result;
}

bool inject_locked() {
    std::lock_guard<std::mutex> lock(g_virtual_bag_mtx);
    if (g_state_entry != nullptr) return true;
    uint8_t* entry = find_inventory_state_entry();
    if (entry == nullptr) return false;
    const uintptr_t expected_event = g_base + fn_resolve("F_SCENE_EVENT_EQUIP_VMA", F_SCENE_EVENT_EQUIP_VMA);
    if (*reinterpret_cast<uintptr_t*>(entry + 0x38) != expected_event) {
        VIRTBAG_LOG("inventory state callbacks differ from expected symbols");
        return false;
    }
    g_orig_event = reinterpret_cast<PopupEventFn>(*reinterpret_cast<uintptr_t*>(entry + 0x38));
    g_orig_f3 = reinterpret_cast<PopupNoArgFn>(*reinterpret_cast<uintptr_t*>(entry + 0x28));

    const uintptr_t bag_draw_call = g_base + fn_resolve("F_UIEQUIP_DRAW_VMA", F_UIEQUIP_DRAW_VMA) + 0x98;
    constexpr uint32_t kOriginalBagDrawCall = 0x97fffee8;
    constexpr size_t kPageSize = 4096;
    if (g_bag_draw_patch_addr == 0) {
        const uintptr_t wrapper = reinterpret_cast<uintptr_t>(&virtual_bag_draw_original_bag_wrapper);
        const int64_t direct_delta = static_cast<int64_t>(wrapper) - static_cast<int64_t>(bag_draw_call);
        uintptr_t branch_target = wrapper;
        if ((direct_delta & 0x3) != 0 || direct_delta <= -0x08000000LL || direct_delta >= 0x08000000LL) {
            g_bag_draw_thunk = allocate_draw_thunk(bag_draw_call, wrapper);
            if (g_bag_draw_thunk == nullptr) {
                VIRTBAG_LOG("inventory bag draw thunk allocation failed");
                return false;
            }
            branch_target = reinterpret_cast<uintptr_t>(g_bag_draw_thunk);
        }
        const int64_t delta = static_cast<int64_t>(branch_target) - static_cast<int64_t>(bag_draw_call);
        if ((delta & 0x3) != 0 || delta <= -0x08000000LL || delta >= 0x08000000LL) {
            VIRTBAG_LOG("inventory bag draw target out of range");
            return false;
        }
        const uint32_t replacement = 0x94000000u | (static_cast<uint32_t>(delta >> 2) & 0x03ffffffu);
        const uintptr_t page = bag_draw_call & ~(static_cast<uintptr_t>(kPageSize) - 1);
        if (mprotect(reinterpret_cast<void*>(page), kPageSize, PROT_READ | PROT_WRITE | PROT_EXEC) != 0) {
            VIRTBAG_LOG("inventory bag draw patch mprotect failed errno=%d", errno);
            return false;
        }
        const uint32_t current = *reinterpret_cast<uint32_t*>(bag_draw_call);
        if (current != kOriginalBagDrawCall && current != replacement) {
            VIRTBAG_LOG("inventory bag draw patch mismatch got=0x%08x", current);
            return false;
        }
        *reinterpret_cast<uint32_t*>(bag_draw_call) = replacement;
        __builtin___clear_cache(reinterpret_cast<char*>(bag_draw_call),
                                reinterpret_cast<char*>(bag_draw_call + sizeof(uint32_t)));
        g_bag_draw_patch_addr = bag_draw_call;
        VIRTBAG_LOG("inventory bag highlight hook patched replacement=0x%08x", replacement);
    }

    const uintptr_t call_addr = g_base + fn_resolve("F_SCENE_DRAW_EQUIP_VMA", F_SCENE_DRAW_EQUIP_VMA) + 0x210;
    constexpr uint32_t kOriginalEndCall = 0x97fd11d2;
    if (g_draw_patch_addr == 0) {
        const uintptr_t wrapper = reinterpret_cast<uintptr_t>(&virtual_bag_draw_end_wrapper);
        const int64_t direct_delta = static_cast<int64_t>(wrapper) - static_cast<int64_t>(call_addr);
        uintptr_t branch_target = wrapper;
        if ((direct_delta & 0x3) != 0 || direct_delta <= -0x08000000LL || direct_delta >= 0x08000000LL) {
            g_draw_thunk = allocate_draw_thunk(call_addr, wrapper);
            if (g_draw_thunk == nullptr) {
                VIRTBAG_LOG("inventory draw wrapper out of BL range and thunk allocation failed call=%p wrapper=%p",
                            reinterpret_cast<void*>(call_addr), reinterpret_cast<void*>(wrapper));
                return false;
            }
            branch_target = reinterpret_cast<uintptr_t>(g_draw_thunk);
        }
        const int64_t delta = static_cast<int64_t>(branch_target) - static_cast<int64_t>(call_addr);
        if ((delta & 0x3) != 0 || delta <= -0x08000000LL || delta >= 0x08000000LL) {
            VIRTBAG_LOG("inventory draw branch target out of BL range call=%p target=%p",
                        reinterpret_cast<void*>(call_addr), reinterpret_cast<void*>(branch_target));
            return false;
        }
        const uint32_t replacement = 0x94000000u | (static_cast<uint32_t>(delta >> 2) & 0x03ffffffu);
        const uintptr_t page = call_addr & ~(static_cast<uintptr_t>(kPageSize) - 1);
        if (mprotect(reinterpret_cast<void*>(page), kPageSize, PROT_READ | PROT_WRITE | PROT_EXEC) != 0) {
            VIRTBAG_LOG("inventory draw patch mprotect failed errno=%d", errno);
            return false;
        }
        const uint32_t current = *reinterpret_cast<uint32_t*>(call_addr);
        if (current != kOriginalEndCall && current != replacement) {
            VIRTBAG_LOG("inventory draw patch mismatch got=0x%08x", current);
            return false;
        }
        *reinterpret_cast<uint32_t*>(call_addr) = replacement;
        __builtin___clear_cache(reinterpret_cast<char*>(call_addr), reinterpret_cast<char*>(call_addr + sizeof(uint32_t)));
        g_draw_patch_addr = call_addr;
        VIRTBAG_LOG("inventory draw restore hook patched call=%p replacement=0x%08x", reinterpret_cast<void*>(call_addr), replacement);
    }
    *reinterpret_cast<uintptr_t*>(entry + 0x28) = reinterpret_cast<uintptr_t>(&virtual_bag_f3_wrapper);
    *reinterpret_cast<uintptr_t*>(entry + 0x38) = reinterpret_cast<uintptr_t>(&virtual_bag_event);
    g_state_entry = entry;
    VIRTBAG_LOG("inventory event callback wrapped; original renderer is reused for module views");
    return true;
}

void ensure_inject_thread() {
    if (g_inject_thread_started.exchange(true)) return;
    std::thread([]() {
        while (g_state_entry == nullptr) {
            inject_locked();
            std::this_thread::sleep_for(std::chrono::milliseconds(300));
        }
    }).detach();
}

}  // namespace

void virtual_bag_ui_start_auto_inject() {
    ensure_inject_thread();
}

void virtual_bag_ui_register_bridge(JNIEnv* env, jclass bridge_class) {
    if (env == nullptr || bridge_class == nullptr) return;
    std::lock_guard<std::mutex> lock(g_virtual_bag_mtx);
    if (g_virtual_bag_bridge_class != nullptr) env->DeleteGlobalRef(g_virtual_bag_bridge_class);
    g_virtual_bag_bridge_class = static_cast<jclass>(env->NewGlobalRef(bridge_class));
}

std::string data_virtual_bag_ui_status_json() {
    std::lock_guard<std::mutex> lock(g_virtual_bag_mtx);
    ensure_state_loaded_locked();
    return "{\"injected\":" + std::string(g_state_entry != nullptr ? "true" : "false") +
           ",\"state\":" + state_json(g_virtual_bag_state) + "}";
}

std::string data_virtual_bag_test_equip(int index, int bag_type) {
    std::lock_guard<std::mutex> lock(g_virtual_bag_mtx);
    ensure_state_loaded_locked();
    if (!virtual_bag::set_test_equipped(&g_virtual_bag_state, index, bag_type)) {
        return op_err("bad virtual bag index or bag type");
    }
    return persist_state_locked() ? op_ok() : op_err("virtual bag save failed");
}

std::string data_virtual_bag_test_item(int index, int slot, int category, int count) {
    std::lock_guard<std::mutex> lock(g_virtual_bag_mtx);
    ensure_state_loaded_locked();
    if (!virtual_bag::set_item(&g_virtual_bag_state, index, slot, category, count)) {
        return op_err("bad virtual bag item");
    }
    return persist_state_locked() ? op_ok() : op_err("virtual bag save failed");
}
