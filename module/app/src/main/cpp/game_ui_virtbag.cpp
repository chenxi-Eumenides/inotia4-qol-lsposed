#include "game_ui_virtbag.h"

#include "game_access.h"
#include "game_inventory.h"
#include "game_ops_common.h"
#include "game_patch.h"
#include "game_state.h"
#include "game_symbols.h"
#include "ownership_ledger.h"
#include "stack_codec.h"
#include "virtual_bag_state.h"
#include "game_ui_kit.h"

#include <android/log.h>

#include <atomic>
#include <cstdarg>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <fcntl.h>
#include <mutex>
#include <sys/mman.h>
#include <thread>
#include <unistd.h>

#define VIRTBAG_TAG "Inotia4VirtBag"

namespace {

std::mutex g_virtbag_log_mtx;

void virtbag_log(const char* format, ...) {
    char message[2048] = {};
    va_list args;
    va_start(args, format);
    std::vsnprintf(message, sizeof(message), format, args);
    va_end(args);
    __android_log_print(ANDROID_LOG_INFO, VIRTBAG_TAG, "%s", message);

    const auto now = std::chrono::system_clock::now();
    const auto since_epoch = now.time_since_epoch();
    const auto milliseconds = std::chrono::duration_cast<std::chrono::milliseconds>(since_epoch).count();
    const std::time_t seconds = std::chrono::system_clock::to_time_t(now);
    std::tm local_time{};
    localtime_r(&seconds, &local_time);
    const int millisecond_part = static_cast<int>(milliseconds % 1000);
    std::lock_guard<std::mutex> lock(g_virtbag_log_mtx);
    const int fd = open("/sdcard/Android/data/com.com2us.inotia4.normal.freefull.google.global.android.common/files/inotia4-export.log",
                        O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0644);
    if (fd < 0) return;
    dprintf(fd, "%04d-%02d-%02d %02d:%02d:%02d.%03d [N] %s\n",
            local_time.tm_year + 1900, local_time.tm_mon + 1, local_time.tm_mday,
            local_time.tm_hour, local_time.tm_min, local_time.tm_sec,
            millisecond_part, message);
    close(fd);
}

#define VIRTBAG_LOG(...) virtbag_log(__VA_ARGS__)

constexpr size_t kPopupStateSize = 0x40;
constexpr int kPopupStateCount = 27;
// 触摸事件使用 Scene_Draw 的绝对逻辑坐标；真机运行时宽度为 1408。
constexpr int64_t kCellX = 0x4c4;
constexpr int64_t kCellY = 0x86;
constexpr int64_t kCellStepY = 0x46;
// Phase one uses an independent read-only overlay. These coordinates stay in
// the same absolute logical space as the original equipment scene.
// UIEquip_CreateInvenControl absolute origin: bag button origin minus the
// bag container offset plus the item-container offset.
constexpr int64_t kGridX = 0x45c - 0x185 + 0x22; // 761
constexpr int64_t kGridY = 0x91 - 0x0c + 0x1b;   // 160
constexpr int64_t kGridCell = 0x4a;
constexpr int64_t kGridStep = 0x53;
constexpr int64_t kOriginalGridX = kGridX;
constexpr int64_t kOriginalGridY = kGridY;
constexpr size_t kInventorySlotStride = 16;
constexpr uint8_t kNoOriginalBagSelected = 6;
// Extension tab ControlObject rects are relative to the item-list root, whose
// absolute origin is (kGridX, kGridY). Their visuals keep the existing overlay
// positions so this phase changes input ownership without moving the UI.
constexpr int64_t kExtensionTabX = kCellX - kGridX;
constexpr int64_t kExtensionTabY = kCellY - kGridY;
constexpr int64_t kExtensionTabWidth = 0x70;
constexpr int64_t kExtensionTabHeight = 0x30;

using PopupEventFn = uint64_t (*)(uint64_t, uint64_t, uint64_t);
using PopupNoArgFn = void (*)();
using OriginalDrawInvenBagFn = void (*)();
using OriginalDrawInvenItemFn = void (*)();

std::mutex g_virtual_bag_mtx;
std::atomic<bool> g_inject_thread_started{false};
std::atomic<bool> g_lifecycle_thread_started{false};
std::atomic<bool> g_virtual_bag_enabled{false};
std::atomic<uint64_t> g_exit_trace_sequence{0};
jclass g_virtual_bag_bridge_class = nullptr;
uint8_t* g_state_entry = nullptr;
PopupEventFn g_orig_event = nullptr;
PopupNoArgFn g_orig_f3 = nullptr;
PopupNoArgFn g_orig_enter = nullptr;
PopupNoArgFn g_orig_save_enter = nullptr;
uint8_t* g_save_state_entry = nullptr;
uintptr_t g_draw_patch_addr = 0;
uintptr_t g_bag_draw_patch_addr = 0;
uintptr_t g_save_inventory_patch_addr = 0;
void* g_save_inventory_thunk = nullptr;
uintptr_t g_drop_gate_patch_addr = 0;
void* g_drop_gate_thunk = nullptr;
uintptr_t g_draw_gate_patch_addr = 0;
void* g_draw_gate_thunk = nullptr;
uintptr_t g_item_draw_patch_addr = 0;
void* g_draw_thunk = nullptr;
void* g_bag_draw_thunk = nullptr;
void* g_item_draw_thunk = nullptr;
virtual_bag::State g_virtual_bag_state{};
int g_loaded_slot = -2;
std::array<std::array<void*, virtual_bag::kSlotCount>, virtual_bag::kBagCount> g_module_objects{};
std::array<std::array<int, virtual_bag::kSlotCount>, virtual_bag::kBagCount> g_module_object_categories{};
std::array<std::array<uint32_t, virtual_bag::kSlotCount>, virtual_bag::kBagCount> g_module_object_hashes{};
std::array<void*, virtual_bag::kSlotCount> g_original_inventory{};
uint8_t g_original_current_direct = 0;
uint8_t g_original_current_got = 0;
uint32_t* g_original_bag_size_word = nullptr;
uint32_t g_original_bag_size = 0;
void* g_projected_item_root = nullptr;
bool g_module_view_installed = false;
int g_module_view_index = -1;
uint8_t g_exit_display_bag = kNoOriginalBagSelected;
bool g_inventory_frame_active = false;
bool g_item_state_dirty = false;
bool g_explicit_save_in_progress = false;
bool g_extension_touch_capture = false;
std::array<void*, virtual_bag::kBagCount> g_extension_tab_buttons{};
void* g_extension_tab_root = nullptr;
uint64_t g_inventory_generation = 0;
uint64_t g_extension_tab_generation = 0;
int g_pending_extension_tab = -1;
uint64_t g_pending_extension_tab_generation = 0;

struct ExtensionDrag {
    bool active = false;
    uint8_t bag = 0;
    uint8_t slot = 0;
    int64_t press_x = 0;
    int64_t press_y = 0;
    int64_t current_x = 0;
    int64_t current_y = 0;
};

ExtensionDrag g_extension_drag{};

void extension_tab_button_clicked(void* ctrl);

int extension_tab_index(void* ctrl) {
    for (int index = 0; index < virtual_bag::kBagCount; ++index) {
        if (g_extension_tab_buttons[index] == ctrl) return index;
    }
    return -1;
}

void install_extension_tab_buttons_locked() {
    if (g_base == 0 || fn_ctrl_btn_create == nullptr) return;
    void* root = *reinterpret_cast<void**>(g_base + G_UIEQUIP_PANEL_CTRL_VMA);
    if (root == nullptr) return;
    if (g_extension_tab_generation == g_inventory_generation &&
        g_extension_tab_root == root && g_extension_tab_buttons[0] != nullptr) {
        return;
    }

    static char texts[virtual_bag::kBagCount][16] = {"扩展1", "扩展2", "扩展3", "扩展4", "扩展5"};
    std::array<void*, virtual_bag::kBagCount> buttons{};
    for (int index = 0; index < virtual_bag::kBagCount; ++index) {
        void* button = fn_ctrl_btn_create(root, texts[index]);
        if (button == nullptr) {
            VIRTBAG_LOG("extension tab create failed index=%d", index);
            return;
        }
        uint8_t* data = *reinterpret_cast<uint8_t**>(static_cast<uint8_t*>(button) + CO_DATA);
        if (data == nullptr) {
            VIRTBAG_LOG("extension tab data is null index=%d", index);
            return;
        }
        *reinterpret_cast<int64_t*>(static_cast<uint8_t*>(button) + CO_RECT_X) = kExtensionTabX;
        *reinterpret_cast<int64_t*>(static_cast<uint8_t*>(button) + CO_RECT_Y) =
            kExtensionTabY + index * kCellStepY;
        *reinterpret_cast<int64_t*>(static_cast<uint8_t*>(button) + CO_RECT_W) = kExtensionTabWidth;
        *reinterpret_cast<int64_t*>(static_cast<uint8_t*>(button) + CO_RECT_H) = kExtensionTabHeight;
        *reinterpret_cast<uintptr_t*>(static_cast<uint8_t*>(button) + CO_PROC) =
            g_base + F_TOUCH_HANDLE_CONTROL_EVENT_PROC_VMA;
        *reinterpret_cast<uintptr_t*>(static_cast<uint8_t*>(button) + CO_CONTROL_PROC) =
            g_base + F_CONTROL_BUTTON_CONTROL_EVENT_PROC_VMA;
        if (fn_ctrl_set_event_call_type != nullptr) fn_ctrl_set_event_call_type(button, 0x200);
        if (fn_ctrl_set_active != nullptr) fn_ctrl_set_active(button, 0x20);
        *reinterpret_cast<void**>(data + CB_EXECUTE_PROC) =
            reinterpret_cast<void*>(&extension_tab_button_clicked);
        *reinterpret_cast<void**>(data + CB_DRAW_PROC) = nullptr;
        if (fn_ctrl_btn_set_text != nullptr) fn_ctrl_btn_set_text(button, texts[index]);
        buttons[index] = button;
    }
    g_extension_tab_buttons = buttons;
    g_extension_tab_root = root;
    g_extension_tab_generation = g_inventory_generation;
    VIRTBAG_LOG("extension tabs installed generation=%llu root=%p",
                static_cast<unsigned long long>(g_inventory_generation), root);
}

void disable_extension_tab_buttons_locked() {
    // The game owns these controls. Invalidate module references without
    // touching them after popup teardown, where the pointers may be stale.
    ++g_inventory_generation;
    g_pending_extension_tab = -1;
    g_pending_extension_tab_generation = 0;
    g_extension_tab_buttons.fill(nullptr);
    g_extension_tab_root = nullptr;
    g_extension_tab_generation = 0;
}

struct UnsavedCrossMove {
    enum class Direction : uint8_t {
        kOriginalToExtension,
        kExtensionToOriginal,
        kExtensionToExtension,
    };
    Direction direction = Direction::kOriginalToExtension;
    uint8_t src_bag = 0;
    uint8_t src_slot = 0;
    uint8_t dst_bag = 0;
    uint8_t dst_slot = virtual_bag::kSlotCount;
    uint8_t extension_bag = 0;
    uint8_t extension_slot = 0;
    uint16_t payload_size = 0;
    std::array<uint8_t, virtual_bag::kSerializedItemBuffer> payload{};
    virtual_bag::Item extension_previous{};
    uint8_t extension_dst_bag = 0;
    uint8_t extension_dst_slot = 0;
    virtual_bag::Item extension_destination_previous{};
};

std::array<UnsavedCrossMove, 128> g_unsaved_cross_moves{};
size_t g_unsaved_cross_move_count = 0;

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

uint8_t* find_popup_state_entry(uintptr_t enter_target) {
    if (g_base == 0) return nullptr;
    void** got = reinterpret_cast<void**>(g_base + G_POPUP_STATE_LIST_GOT_VMA);
    if (got == nullptr || *got == nullptr) return nullptr;
    uint8_t* list = reinterpret_cast<uint8_t*>(*got);
    for (int index = 0; index < kPopupStateCount; ++index) {
        uint8_t* entry = list + index * kPopupStateSize;
        uintptr_t enter = *reinterpret_cast<uintptr_t*>(entry + 0x10);
        if (enter == enter_target) {
            return entry;
        }
    }
    return nullptr;
}

uint8_t* find_inventory_state_entry() {
    return find_popup_state_entry(g_base + fn_resolve("F_PANEL_INVENTORY_ENTER", F_PANEL_INVENTORY_ENTER));
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
    const bool parsed = utf != nullptr && virtual_bag::parse_state_json(utf, &g_virtual_bag_state);
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
    const std::string json = virtual_bag::state_json(g_virtual_bag_state);
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

void ensure_state_loaded_locked();
bool persist_state_locked(bool force = false);
void clear_original_desc_locked();
void clear_original_item_selection_locked();
void rollback_unsaved_cross_moves_locked();
bool extension_tab_hit(int index, int64_t x, int64_t y);
int grid_slot_index(int64_t x, int64_t y, int64_t origin_x, int64_t origin_y);
int original_bag_locked();
void set_original_bag_locked(int bag);
int original_bag_button_index(int64_t x, int64_t y);
bool install_module_view_locked(int bag);
void restore_module_view_locked();
void refresh_projection_if_overwritten_locked();
void* module_item_locked(int bag, int slot);
void recover_pending_transaction_locked();
void prepare_main_menu_locked();
void free_module_object_locked(int bag, int slot);
bool move_original_to_extension_locked(int dst_bag, void* moving_control);
bool move_original_to_extension_slot_locked(int src_bag, int src_slot, int dst_bag);
bool move_extension_to_original_locked(int src_bag, int src_slot, int target_bag);
bool move_extension_to_extension_locked(int src_bag, int src_slot, int dst_bag,
                                        int requested_dst_slot = -1);
bool handle_bag_drop_release_locked(int64_t x, int64_t y);
void* valid_child_locked(void* root, int slot);

// P3：扩展袋切换音效，与原版袋按钮同款（UIEquip_InvenBagControlEventProc b8c34：Play(0x11)）。
void play_extension_switch_sound() {
    if (fn_sound_system_play == nullptr || g_snd_fx == nullptr) return;
    fn_sound_system_play(0x11);
}

// ---- 袋信息页（二次点击袋标签）：容量/物品数/解除按钮，label+button 控件组 ----
void bag_info_unequip_clicked(void* ctrl);
// 解除按钮 = 原版语义：有物品拒绝（静默 v1）、空袋卸下（容量清零+标签禁用）。
// 面板挂 panel root，随面板关闭销毁；切袋/退出/三次点击经显式 remove。
void* g_bag_info_labels[2] = {};
void* g_bag_info_unequip_btn = nullptr;
int g_bag_info_bag = -1;
bool g_bag_info_visible = false;

void remove_bag_info_panel_locked() {
    for (void*& label : g_bag_info_labels) {
        if (label != nullptr && fn_touch_handle_delete_control != nullptr) {
            fn_touch_handle_delete_control(label);
        }
        label = nullptr;
    }
    if (g_bag_info_unequip_btn != nullptr && fn_touch_handle_delete_control != nullptr) {
        fn_touch_handle_delete_control(g_bag_info_unequip_btn);
    }
    g_bag_info_unequip_btn = nullptr;
    g_bag_info_visible = false;
    g_bag_info_bag = -1;
}

void install_bag_info_panel_locked(int bag) {
    remove_bag_info_panel_locked();
    void* root = g_base != 0 ? *reinterpret_cast<void**>(g_base + G_UIEQUIP_PANEL_CTRL_VMA)
                             : nullptr;
    if (root == nullptr || fn_touch_handle_delete_control == nullptr) return;
    char line1[64];
    char line2[64];
    snprintf(line1, sizeof(line1), "扩展背包 %d", bag + 6);
    snprintf(line2, sizeof(line2), "容量 %d", g_virtual_bag_state.capacities[bag]);
    const int row_y = 130 + bag * 70;
    const UiRect r1{470, row_y, 200, 28};
    const UiRect r2{470, row_y + 32, 200, 28};
    const UiRect rb{470, row_y + 64, 200, 36};
    g_bag_info_labels[0] = ui_create_label(root, r1, line1, nullptr);
    g_bag_info_labels[1] = ui_create_label(root, r2, line2, nullptr);
    g_bag_info_unequip_btn =
        ui_create_button(root, rb, "解除", &bag_info_unequip_clicked, nullptr);
    g_bag_info_bag = bag;
    g_bag_info_visible = g_bag_info_labels[0] != nullptr;
}

void bag_info_unequip_clicked(void* ctrl) {
    std::lock_guard<std::mutex> lock(g_virtual_bag_mtx);
    if (!g_bag_info_visible || !virtual_bag::valid_index(g_bag_info_bag)) return;
    const int bag = g_bag_info_bag;
    bool has_items = false;
    for (const auto& item : g_virtual_bag_state.items[bag]) {
        if (item.category > 0 || item.count > 0) {
            has_items = true;
            break;
        }
    }
    if (has_items) {
        VIRTBAG_LOG("bag unequip blocked: bag=%d has items", bag);
        return;
    }
    virtual_bag::unequip_bag(&g_virtual_bag_state, bag);
    remove_bag_info_panel_locked();
    if (g_module_view_installed && g_module_view_index == bag) {
        restore_module_view_locked();
    }
    g_item_state_dirty = true;
    persist_state_locked();
    VIRTBAG_LOG("bag unequipped via info panel bag=%d", bag);
}

void handle_extension_tab_click_locked(int extension_bag) {
    if (!virtual_bag::valid_index(extension_bag)) return;
    ensure_state_loaded_locked();
    if (g_virtual_bag_state.mode == virtual_bag::Mode::kOriginal) {
        const int original_bag = original_bag_locked();
        if (original_bag >= 0 && original_bag < 6) {
            g_original_current_direct = static_cast<uint8_t>(original_bag);
            g_original_current_got = static_cast<uint8_t>(original_bag);
            if (virtual_bag::click(&g_virtual_bag_state, extension_bag) !=
                virtual_bag::ClickResult::kIgnored) {
                install_module_view_locked(extension_bag);
                persist_state_locked();
                VIRTBAG_LOG("extension tab selected bag=%d original_bag=%d", extension_bag,
                            original_bag);
            }
        }
        return;
    }
    if (g_virtual_bag_state.mode == virtual_bag::Mode::kModule) {
        if (g_virtual_bag_state.selected != extension_bag) {
            if (virtual_bag::click(&g_virtual_bag_state, extension_bag) !=
                virtual_bag::ClickResult::kIgnored) {
                install_module_view_locked(extension_bag);
                play_extension_switch_sound();
                VIRTBAG_LOG("extension tab switched bag=%d", extension_bag);
            }
            return;
        }
        // 二次点击 = 袋信息面板 toggle（原版语义：desc_type=1 + MakeDesc）。
        // 面板含容量/物品数/解除按钮；退出模块视图仅经原版袋按钮或 exit_view 端点。
        if (virtual_bag::click(&g_virtual_bag_state, extension_bag) ==
            virtual_bag::ClickResult::kInspected) {
            play_extension_switch_sound();
            if (g_bag_info_visible && g_bag_info_bag == extension_bag) {
                remove_bag_info_panel_locked();
            } else {
                install_bag_info_panel_locked(extension_bag);
            }
            VIRTBAG_LOG("extension bag info opened bag=%d", extension_bag);
        }
    }
}

void queue_extension_tab_click_locked(int extension_bag) {
    if (!virtual_bag::valid_index(extension_bag) ||
        g_extension_tab_generation != g_inventory_generation) {
        return;
    }
    if (g_pending_extension_tab == extension_bag &&
        g_pending_extension_tab_generation == g_inventory_generation) {
        return;
    }
    g_pending_extension_tab = extension_bag;
    g_pending_extension_tab_generation = g_inventory_generation;
    VIRTBAG_LOG("extension tab pending bag=%d generation=%llu", extension_bag,
                static_cast<unsigned long long>(g_inventory_generation));
}

void extension_tab_button_clicked(void* ctrl) {
    std::lock_guard<std::mutex> lock(g_virtual_bag_mtx);
    const int extension_bag = extension_tab_index(ctrl);
    queue_extension_tab_click_locked(extension_bag);
}

void commit_pending_extension_tab_locked() {
    if (!virtual_bag::valid_index(g_pending_extension_tab) ||
        g_pending_extension_tab_generation != g_inventory_generation ||
        g_extension_tab_generation != g_inventory_generation) {
        g_pending_extension_tab = -1;
        g_pending_extension_tab_generation = 0;
        return;
    }
    const int extension_bag = g_pending_extension_tab;
    g_pending_extension_tab = -1;
    g_pending_extension_tab_generation = 0;
    handle_extension_tab_click_locked(extension_bag);
}

void clear_module_cache_locked() {
    for (int bag = 0; bag < virtual_bag::kBagCount; ++bag) {
        for (int slot = 0; slot < virtual_bag::kSlotCount; ++slot) {
            // Scheme C objects never enter g_inven, so the module owns them
            // until an explicit transfer is implemented. Drop them on slot
            // exit instead of retaining stale pointers across saves.
            free_module_object_locked(bag, slot);
        }
    }
    g_original_inventory.fill(nullptr);
    g_original_bag_size_word = nullptr;
    g_original_bag_size = 0;
    g_original_current_direct = 0;
    g_original_current_got = 0;
    g_module_view_installed = false;
    g_module_view_index = -1;
    g_item_state_dirty = false;
    g_unsaved_cross_move_count = 0;
    g_extension_touch_capture = false;
    g_extension_drag = {};
}

bool serialize_item_payload_locked(void* item,
                                   std::array<uint8_t, virtual_bag::kSerializedItemBuffer>* out,
                                   int* out_size) {
    if (item == nullptr || out == nullptr || out_size == nullptr || fn_save_save_item == nullptr) {
        return false;
    }
    std::array<uint8_t, 1024> probe{};
    const int size = fn_save_save_item(probe.data(), item);
    if (size < static_cast<int>(virtual_bag::kPayloadHeaderSize) ||
        size > static_cast<int>(virtual_bag::kMaxSerializedItem)) {
        return false;
    }
    for (size_t index = static_cast<size_t>(size); index < probe.size(); ++index) {
        if (probe[index] != 0) return false;
    }
    out->fill(0);
    std::memcpy(out->data(), probe.data(), static_cast<size_t>(size));
    *out_size = size;
    return true;
}

void ensure_state_loaded_locked() {
    const int slot = current_save_slot();
    if (slot < 0 || slot > 2 || slot == g_loaded_slot) return;
    if (g_module_view_installed) restore_module_view_locked();
    rollback_unsaved_cross_moves_locked();
    clear_module_cache_locked();
    g_virtual_bag_state = {};
    if (!load_state_from_store(slot)) {
        VIRTBAG_LOG("virtual bag state slot=%d unavailable; using empty state", slot);
    }
    g_loaded_slot = slot;
}

// ---- 跨包物品移动事务（v0.7.0）----
// 顺序固定：快照/校验目标 → 内存提交 → 变更原版 → fn_save → sidecar 成功后清理回滚日志。
// 未保存的改动不写入 sidecar；pending 仅保留当前进程内的事务状态。

bool inventory_slot_locked(int bag, int slot, void** out) {
    if (g_inven == nullptr || out == nullptr || bag < 0 || bag >= 6 || slot < 0 ||
        slot >= virtual_bag::kSlotCount) {
        return false;
    }
    *out = static_cast<void**>(g_inven)[bag * kInventorySlotStride + slot];
    return true;
}

bool original_inventory_contains_payload_locked(const uint8_t* payload, int payload_size,
                                                int target_bag) {
    if (g_inven == nullptr || payload == nullptr || fn_save_save_item == nullptr ||
        payload_size <= 0 || target_bag < 0 || target_bag >= 6) {
        return false;
    }
    void** inventory = static_cast<void**>(g_inven);
    for (int slot = 0; slot < virtual_bag::kSlotCount; ++slot) {
        void* item = inventory[target_bag * kInventorySlotStride + slot];
        if (item == nullptr) continue;
        std::array<uint8_t, virtual_bag::kSerializedItemBuffer> serialized{};
        int size = 0;
        if (!serialize_item_payload_locked(item, &serialized, &size) || size != payload_size) continue;
        if (std::memcmp(serialized.data(), payload, static_cast<size_t>(size)) == 0) return true;
    }
    return false;
}

int original_inventory_payload_slot_locked(const uint8_t* payload, int payload_size,
                                           int target_bag) {
    if (g_inven == nullptr || payload == nullptr || fn_save_save_item == nullptr ||
        payload_size <= 0 || target_bag < 0 || target_bag >= 6) {
        return -1;
    }
    void** inventory = static_cast<void**>(g_inven);
    for (int slot = 0; slot < virtual_bag::kSlotCount; ++slot) {
        void* item = inventory[target_bag * kInventorySlotStride + slot];
        if (item == nullptr) continue;
        std::array<uint8_t, virtual_bag::kSerializedItemBuffer> serialized{};
        int size = 0;
        if (serialize_item_payload_locked(item, &serialized, &size) && size == payload_size &&
            std::memcmp(serialized.data(), payload, static_cast<size_t>(size)) == 0) {
            return slot;
        }
    }
    return -1;
}

void record_unsaved_cross_move_locked(const UnsavedCrossMove& move) {
    if (g_unsaved_cross_move_count >= g_unsaved_cross_moves.size()) {
        VIRTBAG_LOG("cross move journal full; refusing rollback record");
        return;
    }
    g_unsaved_cross_moves[g_unsaved_cross_move_count++] = move;
}

void rollback_unsaved_cross_moves_locked() {
    while (g_unsaved_cross_move_count > 0) {
        const UnsavedCrossMove move = g_unsaved_cross_moves[--g_unsaved_cross_move_count];
        bool original_source_restored = true;
        if (move.direction == UnsavedCrossMove::Direction::kOriginalToExtension) {
            void** inventory = static_cast<void**>(g_inven);
            const size_t offset = static_cast<size_t>(move.src_bag) * kInventorySlotStride +
                                  move.src_slot;
            if (inventory == nullptr) {
                original_source_restored = false;
            } else if (inventory[offset] == nullptr && fn_save_load_item != nullptr) {
                void* item = nullptr;
                int consumed = 0;
                if (fn_save_load_item(move.payload.data(), &item, &consumed) == 1 && item != nullptr &&
                    consumed == move.payload_size) {
                    inventory[offset] = item;
                } else if (item != nullptr && fn_itempool_free != nullptr) {
                    fn_itempool_free(item);
                    original_source_restored = false;
                }
            } else if (inventory[offset] == nullptr) {
                original_source_restored = false;
            }
        } else if (move.direction == UnsavedCrossMove::Direction::kExtensionToOriginal) {
            const int slot = original_inventory_payload_slot_locked(
                move.payload.data(), move.payload_size, move.dst_bag);
            int exact_slot = -1;
            if (move.dst_slot < virtual_bag::kSlotCount) {
                void* item = nullptr;
                if (inventory_slot_locked(move.dst_bag, move.dst_slot, &item) && item != nullptr) {
                    exact_slot = move.dst_slot;
                }
            }
            const int rollback_slot = exact_slot >= 0 ? exact_slot : slot;
            if (rollback_slot >= 0 && fn_remove_item_direct != nullptr) {
                fn_remove_item_direct(move.dst_bag, rollback_slot);
                if (fn_ui_equip_refresh_item_area != nullptr) {
                    fn_ui_equip_refresh_item_area();
                }
            }
        } else {
            if (move.extension_bag < virtual_bag::kBagCount &&
                move.extension_slot < virtual_bag::kSlotCount) {
                g_virtual_bag_state.items[move.extension_bag][move.extension_slot] =
                    move.extension_previous;
            }
            if (move.extension_dst_bag < virtual_bag::kBagCount &&
                move.extension_dst_slot < virtual_bag::kSlotCount) {
                g_virtual_bag_state.items[move.extension_dst_bag][move.extension_dst_slot] =
                    move.extension_destination_previous;
            }
        }
        if (move.direction != UnsavedCrossMove::Direction::kExtensionToExtension &&
            (move.direction != UnsavedCrossMove::Direction::kOriginalToExtension ||
             original_source_restored) &&
            move.extension_bag < virtual_bag::kBagCount &&
            move.extension_slot < virtual_bag::kSlotCount) {
            g_virtual_bag_state.items[move.extension_bag][move.extension_slot] =
                move.extension_previous;
        } else if (move.direction == UnsavedCrossMove::Direction::kOriginalToExtension &&
                   !original_source_restored) {
            VIRTBAG_LOG("pending rollback retained extension item: source restore failed src=%u/%u",
                        static_cast<unsigned int>(move.src_bag),
                        static_cast<unsigned int>(move.src_slot));
        }
    }
}

void recover_pending_transaction_locked() {
    const virtual_bag::PendingTransfer pending = g_virtual_bag_state.pending;
    if (!pending.valid) return;
    const bool restore_module = g_module_view_installed;
    const int restore_bag = g_module_view_index;
    if (restore_module) restore_module_view_locked();
    const virtual_bag::RecoveryAction action =
        virtual_bag::recovery_action(g_virtual_bag_state, pending);
    VIRTBAG_LOG("pending recovery direction=%d action=%d", static_cast<int>(pending.direction),
                static_cast<int>(action));
    bool clear_pending = action == virtual_bag::RecoveryAction::kRollback;
    if (action == virtual_bag::RecoveryAction::kComplete) {
        if (pending.direction == virtual_bag::kTransferOriginalToExtension) {
            void* src = nullptr;
            if (!inventory_slot_locked(pending.src_bag, pending.src_slot, &src) || src == nullptr) {
                clear_pending = true;
            } else if (pending.source_payload_size > 0 &&
                       fn_remove_item_direct != nullptr) {
                std::array<uint8_t, virtual_bag::kSerializedItemBuffer> source_payload{};
                int source_size = 0;
                if (!serialize_item_payload_locked(src, &source_payload, &source_size)) {
                    VIRTBAG_LOG("pending recovery: source serialization rejected");
                    clear_pending = false;
                } else {
                    const bool same_source = source_size == pending.source_payload_size &&
                                             std::memcmp(source_payload.data(),
                                                         pending.source_payload.data(),
                                                         pending.source_payload_size) == 0;
                    if (same_source) {
                        fn_remove_item_direct(pending.src_bag, pending.src_slot);
                        if (fn_ui_equip_refresh_item_area != nullptr) {
                            fn_ui_equip_refresh_item_area();
                        }
                        clear_pending = true;
                    } else {
                        VIRTBAG_LOG("pending recovery: source identity mismatch; retain pending");
                    }
                }
            }
        } else {
            if (original_inventory_contains_payload_locked(pending.payload.data(),
                                                            pending.payload_size,
                                                            pending.dst_bag)) {
                clear_pending = true;
            } else {
                set_original_bag_locked(pending.dst_bag);
                void* item = nullptr;
                int consumed = 0;
                bool loaded = fn_save_load_item != nullptr &&
                              fn_save_load_item(pending.payload.data(), &item, &consumed) == 1 &&
                              item != nullptr && consumed == pending.payload_size;
                if (loaded && fn_inven_save_item_on_empty != nullptr &&
                    !fn_inven_save_item_on_empty(item, pending.dst_bag)) {
                    if (fn_itempool_free != nullptr) fn_itempool_free(item);
                    loaded = false;
                }
                if (loaded) {
                    clear_pending = true;
                } else {
                    VIRTBAG_LOG("pending recovery: extension->original retry retained");
                }
            }
        }
    }
    if (clear_pending) {
        g_virtual_bag_state.pending = {};
    }
    persist_state_locked();
    (void)restore_module;
    (void)restore_bag;
}

// P3 所有权账本（ownership_ledger.h）：借出对象四态跟踪。
// materialize → kModuleOwned；install 投影 → kBorrowedForView；restore 收回 → kModuleOwned；
// free → kReleased；原版接管（P4 事务桥接）→ kInventoryOwned。
ownership::Ledger g_ownership_ledger{};
uint32_t g_module_object_handles[virtual_bag::kBagCount][virtual_bag::kSlotCount]{};

// 延迟释放队列：借出对象可能仍被控件 data[0] 引用（Scene_Draw 逐帧解引用），
// 释放后立即 ItemPool_Free 会造成悬空。对象先进隔离区，隔离 N 帧（覆盖一次
// 完整绘制循环）后确认框架不再持有再真释放。
// 借出对象释放策略（根治悬空）：对象可能仍被 TouchState/控件 data 残留引用，
// ItemPool_Free 后这些引用全部悬空（多轮修复未穷尽持有者）。改为**不真释放**：
// 仅清除模块引用，对象内存保留至进程结束（单个 ~48B，开发期可接受；
// P4 对象桥接后借出对象走真实所有权流动，此机制整体退役）。
void defer_item_free_locked(void* item) {
    (void)item;  // 有意不释放：防 TouchState/控件残留引用悬空
}

void free_module_object_locked(int bag, int slot) {
    if (bag < 0 || bag >= virtual_bag::kBagCount || slot < 0 || slot >= virtual_bag::kSlotCount) {
        return;
    }
    void* item = g_module_objects[bag][slot];
    if (item != nullptr && fn_itempool_free != nullptr) {
        // 投影期间先清控件引用：Scene_Draw 会画控件 data[0]，释放悬空对象前
        // 必须置空（否则 ITEM_DrawPorting 解引用已释放内存崩溃，tombstone_06）。
        if (g_module_view_installed && g_module_view_index == bag &&
            g_projected_item_root != nullptr && fn_control_object_get_child != nullptr &&
            fn_control_item_set_item != nullptr) {
            void* ctrl = valid_child_locked(g_projected_item_root, slot);
            if (ctrl != nullptr) fn_control_item_set_item(ctrl, nullptr);
        }
        ownership::release(&g_ownership_ledger, g_module_object_handles[bag][slot]);
        defer_item_free_locked(item);
    }
    g_module_objects[bag][slot] = nullptr;
    g_module_object_categories[bag][slot] = 0;
    g_module_object_hashes[bag][slot] = 0;
    g_module_object_handles[bag][slot] = 0;
}

void* materialize_module_item_locked(const virtual_bag::Item& descriptor) {
    if (virtual_bag::valid_payload(descriptor) &&
        fn_save_load_item != nullptr) {
        void* item = nullptr;
        int consumed = 0;
        const int loaded = fn_save_load_item(descriptor.payload.data(), &item, &consumed);
        if (loaded == 1 && item != nullptr && consumed == descriptor.payload_size) return item;
        if (item != nullptr && fn_itempool_free != nullptr) fn_itempool_free(item);
        VIRTBAG_LOG("module item payload load failed category=%d loaded=%d consumed=%d expected=%u; fallback create",
                    descriptor.category, loaded, consumed,
                    static_cast<unsigned int>(descriptor.payload_size));
    }
    if (fn_create_item == nullptr) return nullptr;
    void* item = fn_create_item(descriptor.category, 0, 0, 0);
    if (item == nullptr) return nullptr;
    uint32_t count_flags = *reinterpret_cast<uint32_t*>(reinterpret_cast<uint8_t*>(item) + I_COUNT);
    *reinterpret_cast<uint32_t*>(reinterpret_cast<uint8_t*>(item) + I_COUNT) =
        stack_codec::write_count(count_flags, static_cast<uint32_t>(descriptor.count));
    return item;
}

void* touch_moving_item_control_locked() {
    if (g_base == 0) return nullptr;
    uint8_t* state = reinterpret_cast<uint8_t*>(g_base + G_TOUCH_STATE_VMA);
    void* control = *reinterpret_cast<void**>(state + TOUCH_STATE_MOVING_CTRL);
    if (control == nullptr) {
        control = *reinterpret_cast<void**>(state + TOUCH_STATE_DROP_SRC_CTRL);
    }
    if (control == nullptr || fn_control_object_get_data == nullptr) return nullptr;
    void* data = fn_control_object_get_data(control);
    if (data == nullptr || *reinterpret_cast<void**>(data) == nullptr) return nullptr;
    if (*reinterpret_cast<uint8_t*>(reinterpret_cast<uint8_t*>(data) + ITEM_CTRL_MOVING_FLAG) == 0) {
        return nullptr;
    }
    return control;
}

void reset_drag_state_locked(void* moving_control) {
    if (g_base != 0) {
        uint8_t* state = reinterpret_cast<uint8_t*>(g_base + G_TOUCH_STATE_VMA);
        std::memset(state, 0, 0x10);
        *reinterpret_cast<void**>(state + 0x10) = nullptr;
        *reinterpret_cast<void**>(state + TOUCH_STATE_MOVING_CTRL) = nullptr;
        *reinterpret_cast<void**>(state + 0x38) = nullptr;
        *reinterpret_cast<void**>(state + 0x50) = nullptr;
        *reinterpret_cast<void**>(state + TOUCH_STATE_DROP_SRC_CTRL) = nullptr;
    }
    if (moving_control != nullptr && fn_control_object_get_data != nullptr) {
        void* data = fn_control_object_get_data(moving_control);
        if (data != nullptr) {
            *reinterpret_cast<uint8_t*>(reinterpret_cast<uint8_t*>(data) + ITEM_CTRL_MOVING_FLAG) = 0;
            *reinterpret_cast<uint8_t*>(reinterpret_cast<uint8_t*>(data) + ITEM_CTRL_ON_FLAG) = 0;
        }
    }
}

void clear_original_item_selection_locked() {
    if (g_base == 0 || fn_ctrl_get_child == nullptr || fn_control_object_get_data == nullptr) {
        return;
    }
    void* item_list = *reinterpret_cast<void**>(g_base + G_UIEQUIP_PANEL_CTRL_VMA);
    if (item_list == nullptr) return;
    for (uint32_t index = 0; index < virtual_bag::kSlotCount; ++index) {
        void* control = fn_ctrl_get_child(item_list, index);
        if (control == nullptr) continue;
        void* data = fn_control_object_get_data(control);
        if (data == nullptr) continue;
        *reinterpret_cast<uint8_t*>(reinterpret_cast<uint8_t*>(data) + ITEM_CTRL_MOVING_FLAG) = 0;
        *reinterpret_cast<uint8_t*>(reinterpret_cast<uint8_t*>(data) + ITEM_CTRL_ON_FLAG) = 0;
    }
}

int original_item_category_locked(void* item) {
    if (item == nullptr) return 0;
    const uint16_t flags = *reinterpret_cast<uint16_t*>(reinterpret_cast<uint8_t*>(item) + I_TYPE);
    return (flags >> 6) & 0x3FF;
}

bool category_is_equip(int category) {
    if (g_base == 0 || category <= 0) return false;
    uint8_t* class_data = *reinterpret_cast<uint8_t**>(
        *reinterpret_cast<void**>(g_base + G_ITEMCLASS_DATA_GOT_VMA));
    uint8_t* size_ptr = *reinterpret_cast<uint8_t**>(g_base + G_ITEMCLASS_SIZE_GOT_VMA);
    if (class_data == nullptr || size_ptr == nullptr) return false;
    const uint8_t stride = *size_ptr;
    if (stride == 0) return false;
    return (class_data[category * stride + 6] & 1) == 0;
}

bool move_original_to_extension_slot_locked(int src_bag, int src_slot, int dst_bag) {
    if (g_unsaved_cross_move_count >= g_unsaved_cross_moves.size()) {
        VIRTBAG_LOG("cross move journal full; rejecting original->extension move");
        return false;
    }
    if (src_bag < 0 || src_bag >= 6 || src_bag == 5) {
        VIRTBAG_LOG("cross move reject original->extension invalid source bag=%d", src_bag);
        return false;
    }
    if (src_slot < 0 || src_slot >= virtual_bag::kSlotCount) {
        VIRTBAG_LOG("cross move reject original->extension invalid source slot=%d bag=%d",
                    src_slot, src_bag);
        return false;
    }
    if (!virtual_bag::valid_index(dst_bag) || g_virtual_bag_state.capacities[dst_bag] == 0 ||
        fn_save_save_item == nullptr || fn_get_cumulate_count == nullptr ||
        fn_remove_item_direct == nullptr) {
        VIRTBAG_LOG("cross move reject original->extension dst=%d capacity=%d symbols=%d",
                    dst_bag,
                    virtual_bag::valid_index(dst_bag) ? g_virtual_bag_state.capacities[dst_bag] : 0,
                    fn_save_save_item != nullptr && fn_get_cumulate_count != nullptr &&
                    fn_remove_item_direct != nullptr ? 1 : 0);
        return false;
    }
    void* src_item = nullptr;
    if (!inventory_slot_locked(src_bag, src_slot, &src_item) || src_item == nullptr) {
        VIRTBAG_LOG("cross move reject original->extension source empty bag=%d slot=%d",
                    src_bag, src_slot);
        return false;
    }

    std::array<uint8_t, virtual_bag::kSerializedItemBuffer> source_payload{};
    int serialized_size = 0;
    if (!serialize_item_payload_locked(src_item, &source_payload, &serialized_size)) {
        VIRTBAG_LOG("cross move: item serialization rejected");
        return false;
    }
    const int src_count = fn_get_cumulate_count(src_item);
    const int src_category = original_item_category_locked(src_item);
    const int capacity = g_virtual_bag_state.capacities[dst_bag];
    virtual_bag::Item source_descriptor{};
    source_descriptor.category = src_category;
    source_descriptor.count = src_count;
    source_descriptor.payload_size = static_cast<uint16_t>(serialized_size);
    source_descriptor.payload = source_payload;

    int dst_slot = -1;
    virtual_bag::Item committed{};
    bool merged = false;
    if (move_merge_enabled()) {
        const uint32_t limit = stack_codec::max_count(stack_limit_enabled());
        for (int slot = 0; slot < capacity && dst_slot < 0; ++slot) {
            const virtual_bag::Item& existing = g_virtual_bag_state.items[dst_bag][slot];
            const uint64_t total = static_cast<uint64_t>(existing.count) +
                                   static_cast<uint64_t>(source_descriptor.count);
            if (!virtual_bag::mergeable_items(existing, source_descriptor) || total > limit) {
                continue;
            }
            committed = existing;
            committed.count = static_cast<int>(virtual_bag::merge_count(
                existing.count, source_descriptor.count, stack_limit_enabled()));
            virtual_bag::patch_payload_count(&committed, static_cast<uint32_t>(committed.count));
            dst_slot = slot;
            merged = true;
        }
    }
    if (dst_slot < 0) {
        for (int slot = 0; slot < capacity; ++slot) {
            const virtual_bag::Item& existing = g_virtual_bag_state.items[dst_bag][slot];
            if (existing.category != 0 || existing.count != 0) continue;
            committed = source_descriptor;
            dst_slot = slot;
            break;
        }
    }
    if (dst_slot < 0) {
        VIRTBAG_LOG("cross move reject original->extension destination full bag=%d capacity=%d",
                    dst_bag, capacity);
        return false;
    }
    if (merged) {
        VIRTBAG_LOG("cross move: original merge src=%d/%d -> extension=%d/%d count=%d+%d=%d",
                    src_bag, src_slot, dst_bag, dst_slot, src_count,
                    g_virtual_bag_state.items[dst_bag][dst_slot].count, committed.count);
    }

    const virtual_bag::Item previous = g_virtual_bag_state.items[dst_bag][dst_slot];
    virtual_bag::PendingTransfer pending{};
    pending.valid = true;
    pending.direction = virtual_bag::kTransferOriginalToExtension;
    pending.src_bag = static_cast<uint8_t>(src_bag);
    pending.src_slot = static_cast<uint8_t>(src_slot);
    pending.dst_bag = static_cast<uint8_t>(dst_bag);
    pending.dst_slot = static_cast<uint8_t>(dst_slot);
    pending.payload_size = committed.payload_size;
    pending.payload = committed.payload;
    pending.source_payload_size = static_cast<uint16_t>(serialized_size);
    pending.source_payload = source_payload;

    g_virtual_bag_state.pending = pending;
    g_item_state_dirty = true;
    if (!persist_state_locked(true)) {
        g_virtual_bag_state.pending = {};
        return false;
    }
    g_virtual_bag_state.items[dst_bag][dst_slot] = committed;
    if (!persist_state_locked(true)) {
        g_virtual_bag_state.items[dst_bag][dst_slot] = previous;
        g_virtual_bag_state.pending = {};
        persist_state_locked();
        return false;
    }
    if (module_item_locked(dst_bag, dst_slot) == nullptr) {
        g_virtual_bag_state.items[dst_bag][dst_slot] = previous;
        g_virtual_bag_state.pending = {};
        free_module_object_locked(dst_bag, dst_slot);
        VIRTBAG_LOG("cross move: target extension item could not be materialized; source retained");
        return false;
    }
    if (fn_remove_item_direct == nullptr) {
        g_virtual_bag_state.items[dst_bag][dst_slot] = previous;
        g_virtual_bag_state.pending = {};
        persist_state_locked(true);
        VIRTBAG_LOG("cross move: original->extension source removal failed");
        return false;
    }
    fn_remove_item_direct(src_bag, src_slot);
    void* remaining_source = nullptr;
    if (inventory_slot_locked(src_bag, src_slot, &remaining_source) && remaining_source != nullptr) {
        g_virtual_bag_state.items[dst_bag][dst_slot] = previous;
        g_virtual_bag_state.pending = {};
        persist_state_locked(true);
        VIRTBAG_LOG("cross move: original->extension source slot remained occupied");
        return false;
    }
    UnsavedCrossMove unsaved_move{};
    unsaved_move.direction = UnsavedCrossMove::Direction::kOriginalToExtension;
    unsaved_move.src_bag = static_cast<uint8_t>(src_bag);
    unsaved_move.src_slot = static_cast<uint8_t>(src_slot);
    unsaved_move.extension_bag = static_cast<uint8_t>(dst_bag);
    unsaved_move.extension_slot = static_cast<uint8_t>(dst_slot);
    unsaved_move.payload_size = static_cast<uint16_t>(serialized_size);
    unsaved_move.payload = source_payload;
    unsaved_move.extension_previous = previous;
    record_unsaved_cross_move_locked(unsaved_move);
    g_item_state_dirty = true;
    g_virtual_bag_state.pending = {};
    if (!persist_state_locked()) {
        VIRTBAG_LOG("cross move: pending clear persist failed; will recover on next load");
    }
    g_virtual_bag_state.mode = virtual_bag::Mode::kModule;
    g_virtual_bag_state.selected = dst_bag;
    g_virtual_bag_state.inspected = -1;
    free_module_object_locked(dst_bag, dst_slot);
    if (fn_ui_equip_refresh_item_area != nullptr) fn_ui_equip_refresh_item_area();
    VIRTBAG_LOG("cross move: original->extension bag=%d slot=%d cat=%d count=%d src=%d/%d",
                dst_bag, dst_slot, src_category, committed.count, src_bag, src_slot);
    return true;
}

bool move_original_to_extension_locked(int dst_bag, void* moving_control) {
    if (!virtual_bag::valid_index(dst_bag) || g_virtual_bag_state.capacities[dst_bag] == 0 ||
        fn_ui_equip_get_item_slot_index == nullptr) {
        VIRTBAG_LOG("cross move reject original->extension dst=%d control=%p capacity=%d symbol=%d",
                    dst_bag, moving_control,
                    virtual_bag::valid_index(dst_bag) ? g_virtual_bag_state.capacities[dst_bag] : 0,
                    fn_ui_equip_get_item_slot_index != nullptr ? 1 : 0);
        return false;
    }
    const int src_bag = original_bag_locked();
    const int src_slot = fn_ui_equip_get_item_slot_index(moving_control);
    const bool moved = move_original_to_extension_slot_locked(src_bag, src_slot, dst_bag);
    if (moved) reset_drag_state_locked(moving_control);
    return moved;
}

bool move_extension_to_original_locked(int src_bag, int src_slot, int target_bag) {
    if (target_bag < 0 || target_bag >= 6 || target_bag == 5 || !virtual_bag::valid_index(src_bag) ||
        src_slot < 0 || src_slot >= virtual_bag::kSlotCount ||
        fn_inven_save_item_on_empty == nullptr) {
        VIRTBAG_LOG("cross move reject extension->original src=%d/%d target_bag=%d", src_bag,
                    src_slot, target_bag);
        return false;
    }
    if (g_unsaved_cross_move_count >= g_unsaved_cross_moves.size()) {
        VIRTBAG_LOG("cross move journal full; rejecting extension->original move");
        return false;
    }
    if (fn_get_bag_size != nullptr && fn_get_bag_size(target_bag) <= 0) {
        VIRTBAG_LOG("cross move reject extension->original target bag unavailable=%d", target_bag);
        return false;
    }
    virtual_bag::Item source = g_virtual_bag_state.items[src_bag][src_slot];
    if (source.category <= 0 || source.count <= 0) {
        VIRTBAG_LOG("cross move reject extension->original source empty=%d/%d", src_bag,
                    src_slot);
        return false;
    }
    if (source.payload_size == 0) {
        void* source_object = module_item_locked(src_bag, src_slot);
        if (source_object == nullptr || fn_save_save_item == nullptr) {
            VIRTBAG_LOG("cross move reject extension->original source object unavailable=%d/%d",
                        src_bag, src_slot);
            return false;
        }
        std::array<uint8_t, virtual_bag::kSerializedItemBuffer> payload{};
        int serialized_size = 0;
        if (!serialize_item_payload_locked(source_object, &payload, &serialized_size)) {
            VIRTBAG_LOG("cross move reject extension->original serialization failed=%d/%d",
                        src_bag, src_slot);
            return false;
        }
        source.payload_size = static_cast<uint16_t>(serialized_size);
        source.payload = payload;
    }

    virtual_bag::PendingTransfer pending{};
    pending.valid = true;
    pending.direction = virtual_bag::kTransferExtensionToOriginal;
    pending.src_bag = static_cast<uint8_t>(src_bag);
    pending.src_slot = static_cast<uint8_t>(src_slot);
    pending.dst_bag = static_cast<uint8_t>(target_bag);
    pending.dst_slot = 0;
    pending.payload_size = source.payload_size;
    pending.payload = source.payload;

    g_virtual_bag_state.pending = pending;
    g_item_state_dirty = true;
    if (!persist_state_locked(true)) {
        g_virtual_bag_state.pending = {};
        VIRTBAG_LOG("cross move extension->original pending persist failed src=%d/%d target=%d",
                    src_bag, src_slot, target_bag);
        return false;
    }
    const virtual_bag::Item previous = g_virtual_bag_state.items[src_bag][src_slot];
    g_virtual_bag_state.items[src_bag][src_slot] = {};
    g_item_state_dirty = true;
    if (!persist_state_locked(true)) {
        g_virtual_bag_state.items[src_bag][src_slot] = previous;
        g_virtual_bag_state.pending = {};
        persist_state_locked();
        VIRTBAG_LOG("cross move extension->original source clear persist failed src=%d/%d",
                    src_bag, src_slot);
        return false;
    }

    free_module_object_locked(src_bag, src_slot);
    set_original_bag_locked(target_bag);

    void* item = nullptr;
    int consumed = 0;
    if (source.payload_size >= virtual_bag::kPayloadHeaderSize && fn_save_load_item != nullptr &&
        fn_save_load_item(source.payload.data(), &item, &consumed) == 1 && item != nullptr &&
        consumed == source.payload_size) {
        // 无损路径：SAVE_LoadItem 重建
    } else if (source.payload_size == 0 && fn_create_item != nullptr) {
        item = fn_create_item(source.category, 0, 0, 0);
        if (item != nullptr) {
            uint32_t count_flags =
                *reinterpret_cast<uint32_t*>(reinterpret_cast<uint8_t*>(item) + I_COUNT);
            *reinterpret_cast<uint32_t*>(reinterpret_cast<uint8_t*>(item) + I_COUNT) =
                stack_codec::write_count(count_flags, static_cast<uint32_t>(source.count));
        }
    }
    if (item == nullptr || !fn_inven_save_item_on_empty(item, target_bag)) {
        if (item != nullptr && fn_itempool_free != nullptr) fn_itempool_free(item);
        g_virtual_bag_state.items[src_bag][src_slot] = previous;
        g_virtual_bag_state.pending = {};
        persist_state_locked();
        reset_drag_state_locked(nullptr);
        VIRTBAG_LOG("cross move: extension->original target insertion failed; rollback src=%d/%d target=%d",
                    src_bag, src_slot, target_bag);
        return false;
    }
    UnsavedCrossMove unsaved_move{};
    unsaved_move.direction = UnsavedCrossMove::Direction::kExtensionToOriginal;
    unsaved_move.src_bag = static_cast<uint8_t>(src_bag);
    unsaved_move.src_slot = static_cast<uint8_t>(src_slot);
    unsaved_move.dst_bag = static_cast<uint8_t>(target_bag);
    void* inserted_item = nullptr;
    for (int slot = 0; slot < virtual_bag::kSlotCount; ++slot) {
        if (inventory_slot_locked(target_bag, slot, &inserted_item) && inserted_item == item) {
            unsaved_move.dst_slot = static_cast<uint8_t>(slot);
            break;
        }
    }
    if (unsaved_move.dst_slot >= virtual_bag::kSlotCount) {
        const int matched_slot = original_inventory_payload_slot_locked(
            source.payload.data(), source.payload_size, target_bag);
        if (matched_slot >= 0) unsaved_move.dst_slot = static_cast<uint8_t>(matched_slot);
    }
    if (unsaved_move.dst_slot >= virtual_bag::kSlotCount) {
        VIRTBAG_LOG("cross move: extension->original insertion slot not found; rolling back");
        const int matched_slot = original_inventory_payload_slot_locked(
            source.payload.data(), source.payload_size, target_bag);
        if (matched_slot >= 0 && fn_remove_item_direct != nullptr) {
            fn_remove_item_direct(target_bag, matched_slot);
            if (fn_ui_equip_refresh_item_area != nullptr) {
                fn_ui_equip_refresh_item_area();
            }
        }
        g_virtual_bag_state.items[src_bag][src_slot] = previous;
        g_virtual_bag_state.pending = {};
        persist_state_locked();
        reset_drag_state_locked(nullptr);
        return false;
    }
    unsaved_move.extension_bag = static_cast<uint8_t>(src_bag);
    unsaved_move.extension_slot = static_cast<uint8_t>(src_slot);
    unsaved_move.payload_size = source.payload_size;
    unsaved_move.payload = source.payload;
    unsaved_move.extension_previous = previous;
    record_unsaved_cross_move_locked(unsaved_move);
    g_virtual_bag_state.pending = {};
    g_virtual_bag_state.mode = virtual_bag::Mode::kModule;
    g_virtual_bag_state.selected = src_bag;
    g_virtual_bag_state.inspected = -1;
    set_original_bag_locked(target_bag);
    persist_state_locked();
    reset_drag_state_locked(nullptr);
    VIRTBAG_LOG("cross move: extension->original src=%d/%d cat=%d count=%d target_bag=%d",
                src_bag, src_slot, source.category, source.count, target_bag);
    return true;
}

bool move_extension_to_extension_locked(int src_bag, int src_slot, int dst_bag,
                                        int requested_dst_slot) {
    if (!virtual_bag::valid_index(src_bag) || !virtual_bag::valid_index(dst_bag) ||
        g_virtual_bag_state.capacities[dst_bag] == 0 || src_bag == dst_bag ||
        src_slot < 0 || src_slot >= virtual_bag::kSlotCount) {
        VIRTBAG_LOG("cross move reject extension->extension src=%d/%d dst_bag=%d requested_dst=%d",
                    src_bag, src_slot, dst_bag, requested_dst_slot);
        return false;
    }
    const virtual_bag::Item& source = g_virtual_bag_state.items[src_bag][src_slot];
    if (source.category <= 0 || source.count <= 0) {
        VIRTBAG_LOG("cross move reject extension->extension source empty=%d/%d", src_bag,
                    src_slot);
        return false;
    }

    const int capacity = g_virtual_bag_state.capacities[dst_bag];

    int dst_slot = -1;
    virtual_bag::Item committed{};
    bool merged = false;
    auto try_merge = [&](int slot) {
        const virtual_bag::Item& existing = g_virtual_bag_state.items[dst_bag][slot];
        if (!move_merge_enabled() || !virtual_bag::mergeable_items(existing, source)) return false;
        const uint32_t limit = stack_codec::max_count(stack_limit_enabled());
        const uint64_t total = static_cast<uint64_t>(existing.count) +
                               static_cast<uint64_t>(source.count);
        if (total > limit) return false;
        committed = existing;
        committed.count = static_cast<int>(virtual_bag::merge_count(
            existing.count, source.count, stack_limit_enabled()));
        virtual_bag::patch_payload_count(&committed, static_cast<uint32_t>(committed.count));
        dst_slot = slot;
        merged = true;
        return true;
    };
    if (requested_dst_slot >= 0) {
        if (requested_dst_slot >= capacity) {
            VIRTBAG_LOG("cross move reject extension->extension invalid destination=%d capacity=%d",
                        requested_dst_slot, capacity);
            return false;
        }
        const virtual_bag::Item& existing = g_virtual_bag_state.items[dst_bag][requested_dst_slot];
        if (!try_merge(requested_dst_slot)) {
            if (existing.category != 0 || existing.count != 0) {
                VIRTBAG_LOG("cross move reject extension->extension destination occupied=%d/%d",
                            dst_bag, requested_dst_slot);
                return false;
            }
            committed = source;
            dst_slot = requested_dst_slot;
        }
    } else if (move_merge_enabled()) {
        for (int slot = 0; slot < capacity && dst_slot < 0; ++slot) {
            try_merge(slot);
        }
    }
    if (dst_slot < 0) {
        for (int slot = 0; slot < capacity; ++slot) {
            const virtual_bag::Item& existing = g_virtual_bag_state.items[dst_bag][slot];
            if (existing.category != 0 || existing.count != 0) continue;
            committed = source;
            dst_slot = slot;
            break;
        }
    }
    if (dst_slot < 0) {
        VIRTBAG_LOG("cross move reject extension->extension destination full=%d capacity=%d",
                    dst_bag, capacity);
        return false;
    }

    if (merged) {
        VIRTBAG_LOG("cross move: extension merge %d/%d -> %d/%d count=%d+%d=%d",
                    src_bag, src_slot, dst_bag, dst_slot, source.count,
                    g_virtual_bag_state.items[dst_bag][dst_slot].count, committed.count);
    }

    if (g_unsaved_cross_move_count >= g_unsaved_cross_moves.size()) {
        VIRTBAG_LOG("cross move journal full; rejecting extension->extension move");
        return false;
    }
    const virtual_bag::Item previous_dst = g_virtual_bag_state.items[dst_bag][dst_slot];
    const virtual_bag::Item previous_src = g_virtual_bag_state.items[src_bag][src_slot];
    g_virtual_bag_state.items[src_bag][src_slot] = {};
    g_virtual_bag_state.items[dst_bag][dst_slot] = committed;
    g_item_state_dirty = true;
    if (!persist_state_locked()) {
        g_virtual_bag_state.items[src_bag][src_slot] = previous_src;
        g_virtual_bag_state.items[dst_bag][dst_slot] = previous_dst;
        persist_state_locked();
        VIRTBAG_LOG("cross move extension->extension persist failed src=%d/%d dst=%d/%d",
                    src_bag, src_slot, dst_bag, dst_slot);
        return false;
    }
    UnsavedCrossMove unsaved_move{};
    unsaved_move.direction = UnsavedCrossMove::Direction::kExtensionToExtension;
    unsaved_move.extension_bag = static_cast<uint8_t>(src_bag);
    unsaved_move.extension_slot = static_cast<uint8_t>(src_slot);
    unsaved_move.extension_previous = previous_src;
    unsaved_move.extension_dst_bag = static_cast<uint8_t>(dst_bag);
    unsaved_move.extension_dst_slot = static_cast<uint8_t>(dst_slot);
    unsaved_move.extension_destination_previous = previous_dst;
    record_unsaved_cross_move_locked(unsaved_move);
    free_module_object_locked(src_bag, src_slot);
    free_module_object_locked(dst_bag, dst_slot);
    reset_drag_state_locked(nullptr);
    VIRTBAG_LOG("cross move: extension->extension %d/%d -> %d/%d cat=%d count=%d",
                src_bag, src_slot, dst_bag, dst_slot, source.category, committed.count);
    return true;
}

bool handle_bag_drop_release_locked(int64_t x, int64_t y) {
    ensure_state_loaded_locked();
    recover_pending_transaction_locked();
    VIRTBAG_LOG("drop release mode=%d selected=%d x=%lld y=%lld moving=%p extension_drag=%d",
                static_cast<int>(g_virtual_bag_state.mode), g_virtual_bag_state.selected,
                static_cast<long long>(x), static_cast<long long>(y),
                touch_moving_item_control_locked(), g_extension_drag.active ? 1 : 0);
    if (g_virtual_bag_state.mode == virtual_bag::Mode::kOriginal) {
        void* moving_control = touch_moving_item_control_locked();
        if (moving_control == nullptr) {
            VIRTBAG_LOG("drop reject original->extension no moving original control");
            return false;
        }
        for (int index = 0; index < virtual_bag::kBagCount; ++index) {
            if (!extension_tab_hit(index, x, y)) continue;
            return move_original_to_extension_locked(index, moving_control);
        }
        VIRTBAG_LOG("drop reject original->extension no extension tab hit x=%lld y=%lld",
                    static_cast<long long>(x), static_cast<long long>(y));
        return false;
    }
    if (g_virtual_bag_state.mode == virtual_bag::Mode::kModule && g_extension_drag.active) {
        const int target_slot = grid_slot_index(x, y, kGridX, kGridY);
            if (target_slot >= 0) {
            const bool handled = move_extension_to_extension_locked(
                g_extension_drag.bag, g_extension_drag.slot,
                g_virtual_bag_state.selected, target_slot);
            VIRTBAG_LOG("drop result direction=extension->extension handled=%d src=%d/%d dst=%d/%d",
                        handled ? 1 : 0, g_extension_drag.bag, g_extension_drag.slot,
                        g_virtual_bag_state.selected, target_slot);
            return handled;
        }
        for (int index = 0; index < virtual_bag::kBagCount; ++index) {
            if (index == g_extension_drag.bag) continue;
            if (!extension_tab_hit(index, x, y)) continue;
            const bool handled = move_extension_to_extension_locked(
                g_extension_drag.bag, g_extension_drag.slot, index, -1);
            VIRTBAG_LOG("drop result direction=extension->extension handled=%d src=%d/%d dst_bag=%d",
                        handled ? 1 : 0, g_extension_drag.bag, g_extension_drag.slot, index);
            return handled;
        }
        const int target_bag = original_bag_button_index(x, y);
        if (target_bag >= 0 && target_bag < 6) {
            if (target_bag == 5) {
                // 索引 5 = 原版任务袋（ADR-006）：不得成为扩展移动目标（触摸路径与 API 门禁对齐）。
                VIRTBAG_LOG("drop reject extension->original target=task bag(5)");
                return false;
            }
            const bool handled = move_extension_to_original_locked(
                g_extension_drag.bag, g_extension_drag.slot, target_bag);
            VIRTBAG_LOG("drop result direction=extension->original handled=%d src=%d/%d target_bag=%d",
                        handled ? 1 : 0, g_extension_drag.bag, g_extension_drag.slot, target_bag);
            return handled;
        }
        VIRTBAG_LOG("drop reject extension destination not found x=%lld y=%lld",
                    static_cast<long long>(x), static_cast<long long>(y));
    } else if (g_virtual_bag_state.mode == virtual_bag::Mode::kModule) {
        VIRTBAG_LOG("drop reject extension no active drag selected=%d",
                    g_virtual_bag_state.selected);
    }
    return false;
}

bool persist_state_locked(bool force) {
    (void)force;
    if (!g_explicit_save_in_progress) return true;
    const int slot = current_save_slot();
    if (slot < 0 || slot > 2) return false;
    if (!save_state_to_store(slot)) {
        VIRTBAG_LOG("virtual bag state slot=%d save failed", slot);
        return false;
    }
    g_loaded_slot = slot;
    if (g_explicit_save_in_progress) g_item_state_dirty = false;
    return true;
}

bool extension_tab_hit(int index, int64_t x, int64_t y) {
    if (!virtual_bag::valid_index(index)) return false;
    if (g_virtual_bag_state.capacities[index] == 0) return false;  // 未装备袋不可命中（G-12）
    const int64_t tab_y = kCellY + index * kCellStepY;
    return x >= kCellX && x < kCellX + kExtensionTabWidth &&
           y >= tab_y && y < tab_y + kExtensionTabHeight;
}

// GetChild 结果必须过双重校验：面板重建窗口里 GetChild(slot≥count) 返回不可读的
// 垃圾指针（tombstone_02），先 GetCount 判界消除越界源，再 GetUserType==2 验类型。
// root 本身也可能被面板重建替换——调用方应实时读 *(G_UIEQUIP_PANEL_CTRL_VMA)，
// 不要使用 install 时的快照。
void* valid_child_locked(void* root, int slot) {
    if (root == nullptr || slot < 0 || fn_ctrl_get_count == nullptr ||
        fn_control_object_get_child == nullptr) {
        return nullptr;
    }
    if (static_cast<int>(fn_ctrl_get_count(root)) <= slot) return nullptr;
    void* ctrl = fn_control_object_get_child(root, static_cast<uint32_t>(slot));
    if (ctrl == nullptr || fn_control_object_get_user_type == nullptr ||
        fn_control_object_get_user_type(ctrl) != 2) {
        return nullptr;
    }
    return ctrl;
}

// ---- G-8 投影拖动状态机（模块侧，独立于 TouchHandle 内部拖动）----
// press 只读记录命中扩展槽（不触碰 TouchHandle）；move 更新触点 + 吞 panel 层事件
// （抑制 TouchHandle 拖动与场景溢出）；drop 在控件层路由到既有跨包事务：
//   落到物品控件（0x02）→ ext→ext；落到袋控件（0x04）→ ext→orig（经 SaveItemOnEmpty 门禁）。
// G-6：投影命中走控件 AbsoluteRect。GetAbsoluteRect 是 x8 sret 函数禁止 C++ 直调
// （真机 SIGSEGV），用手工父链累加（ctrl_abs_point 同款，纯内存读）。
// w/h 用贴图固定尺寸 kGridCell（GetAbsoluteRect 也只输出 x/y 两个 i64）。
// 物品指针合法性：模块缓存、INVEN_pItem 两集合任一命中即合法
// （借出对象不再真释放，无需隔离区）。
bool item_pointer_known_locked(void* item) {
    if (item == nullptr) return true;  // 空指针由调用方各自处理
    for (int bag = 0; bag < virtual_bag::kBagCount; ++bag) {
        for (int slot = 0; slot < virtual_bag::kSlotCount; ++slot) {
            if (g_module_objects[bag][slot] == item) return true;
        }
    }
    if (g_inven != nullptr) {
        void** inventory = static_cast<void**>(g_inven);
        for (int idx = 0; idx < 6 * virtual_bag::kSlotCount; ++idx) {
            if (inventory[idx] == item) return true;
        }
    }
    return false;
}

bool extension_grid_hit(int64_t x, int64_t y) {
    if (g_virtual_bag_state.mode == virtual_bag::Mode::kOriginal ||
        !virtual_bag::valid_index(g_virtual_bag_state.selected)) {
        return false;
    }
    const int64_t grid_width = 4 * kGridStep - (kGridStep - kGridCell);
    const int64_t grid_height = 4 * kGridStep - (kGridStep - kGridCell);
    return x >= kGridX && x < kGridX + grid_width && y >= kGridY && y < kGridY + grid_height;
}

int grid_slot_index(int64_t x, int64_t y, int64_t origin_x, int64_t origin_y) {
    if (x < origin_x || y < origin_y) return -1;
    const int64_t relative_x = x - origin_x;
    const int64_t relative_y = y - origin_y;
    const int column = static_cast<int>(relative_x / kGridStep);
    const int row = static_cast<int>(relative_y / kGridStep);
    if (column < 0 || column >= 4 || row < 0 || row >= 4) return -1;
    if (relative_x % kGridStep >= kGridCell || relative_y % kGridStep >= kGridCell) return -1;
    return row * 4 + column;
}

void draw_cells_in_frame_locked() {
    const bool can_draw_original_button = fn_grpx_draw_part != nullptr && fn_imgsys_get_group != nullptr &&
                                           fn_imgsys_get_loc != nullptr;
    void* group = can_draw_original_button ? fn_imgsys_get_group(0xf) : nullptr;
    void* current_root = g_base != 0 ? *reinterpret_cast<void**>(g_base + G_UIEQUIP_PANEL_CTRL_VMA) : nullptr;
    const bool tabs_are_current = current_root != nullptr && current_root == g_extension_tab_root &&
                                  g_extension_tab_generation == g_inventory_generation;
    for (int index = 0; index < virtual_bag::kBagCount; ++index) {
        void* button = tabs_are_current ? g_extension_tab_buttons[index] : nullptr;
        if (button != nullptr) {
            const bool selected = g_virtual_bag_state.mode == virtual_bag::Mode::kModule &&
                                  g_virtual_bag_state.selected == index;
            const bool equipped = g_virtual_bag_state.capacities[index] != 0;
            VIRTBAG_LOG("tab draw index=%d selected=%d equipped=%d", index,
                        selected ? 1 : 0, equipped ? 1 : 0);
            // G-1：贴图对齐 DrawInvenBag 原版语义——选中=loc 0x13（亮）、
            // 装备未选中=loc 0xa（暗）、未装备=loc 9（空袋）；无边框，选中/未选中靠贴图区分。
            if (can_draw_original_button && group != nullptr) {
                int64_t ax = 0, ay = 0;
                uint8_t* c = reinterpret_cast<uint8_t*>(button);
                ax = *reinterpret_cast<int64_t*>(c + CO_RECT_X);
                ay = *reinterpret_cast<int64_t*>(c + CO_RECT_Y);
                void* p = *reinterpret_cast<void**>(c + CO_PARENT);
                while (p != nullptr) {
                    uint8_t* pc = reinterpret_cast<uint8_t*>(p);
                    ax += *reinterpret_cast<int64_t*>(pc + CO_RECT_X);
                    ay += *reinterpret_cast<int64_t*>(pc + CO_RECT_Y);
                    p = *reinterpret_cast<void**>(pc + CO_PARENT);
                }
                // 贴图照抄 DrawInvenBag 原版分支（b73c0-b7444），含 w6 差异：
                // 选中=先 loc 0x13（w6=0）再 loc 6（w6=0，"亮底框"）；
                // 未选中装备=仅 loc 6（w6=0x28，"暗底框"）；未装备=仅 loc 9（w6=0x28）。
                if (selected) {
                    void* icon = fn_imgsys_get_loc(0xf, 0x13);
                    if (icon != nullptr) {
                        fn_grpx_draw_part(group, static_cast<int32_t>(ax - 10),
                                          static_cast<int32_t>(ay - 3), icon, 0, 1, 0);
                    }
                    void* frame = fn_imgsys_get_loc(0xf, 6);
                    if (frame != nullptr) {
                        fn_grpx_draw_part(group, static_cast<int32_t>(ax),
                                          static_cast<int32_t>(ay), frame, 0, 1, 0);
                    }
                } else if (equipped) {
                    void* frame = fn_imgsys_get_loc(0xf, 6);
                    if (frame != nullptr) {
                        fn_grpx_draw_part(group, static_cast<int32_t>(ax),
                                          static_cast<int32_t>(ay), frame, 0, 1, 0x28);
                    }
                } else {
                    void* bag_icon = fn_imgsys_get_loc(0xf, 9);
                    if (bag_icon != nullptr) {
                        fn_grpx_draw_part(group, static_cast<int32_t>(ax),
                                          static_cast<int32_t>(ay), bag_icon, 0, 1, 0x28);
                    }
                }
            }
            ui_draw_text_centered(button, 8, equipped ? 0xffffffffu : 0xff888888u);
        }
    }

    if (g_virtual_bag_state.mode != virtual_bag::Mode::kModule ||
        !virtual_bag::valid_index(g_virtual_bag_state.selected) || g_module_view_installed) {
        return;
    }

    const int bag = g_virtual_bag_state.selected;
    const int capacity = g_virtual_bag_state.capacities[bag];
    for (int slot = 0; slot < capacity; ++slot) {
        const int column = slot % 4;
        const int row = slot / 4;
        const UiRect rect{kGridX + column * kGridStep, kGridY + row * kGridStep,
                          kGridCell, kGridCell};
        const bool occupied = slot < capacity &&
                              g_virtual_bag_state.items[bag][slot].category > 0 &&
                              g_virtual_bag_state.items[bag][slot].count > 0;
        const bool dragging_source = g_extension_drag.active &&
                                     g_extension_drag.bag == bag &&
                                     g_extension_drag.slot == slot;
        if (can_draw_original_button && group != nullptr) {
            void* slot_background = fn_imgsys_get_loc(0xf, 0x29);
            if (slot_background != nullptr) {
                fn_grpx_draw_part(group, static_cast<int32_t>(rect.x), static_cast<int32_t>(rect.y),
                                  slot_background, 0, 1, 0);
            }
        }
        if (occupied && !dragging_source && fn_item_draw_porting != nullptr) {
            void* item = module_item_locked(bag, slot);
            if (item != nullptr) {
                // ITEM_DrawPorting applies the original +5 inset, rarity
                // frame, icon mapping and stack count itself.
                fn_item_draw_porting(item, static_cast<int32_t>(rect.x),
                                     static_cast<int32_t>(rect.y), 0, 1);
            }
        }
        if (g_virtual_bag_state.inspected == slot && fn_grpx_fill_rect != nullptr) {
            constexpr int32_t kInspectBorder = 3;
            constexpr uint32_t kInspectColor = 0xff75d8ff;
            fn_grpx_fill_rect(static_cast<int32_t>(rect.x), static_cast<int32_t>(rect.y),
                              kGridCell, kInspectBorder, kInspectColor);
            fn_grpx_fill_rect(static_cast<int32_t>(rect.x),
                              static_cast<int32_t>(rect.y + kGridCell - kInspectBorder),
                              kGridCell, kInspectBorder, kInspectColor);
            fn_grpx_fill_rect(static_cast<int32_t>(rect.x), static_cast<int32_t>(rect.y),
                              kInspectBorder, kGridCell, kInspectColor);
            fn_grpx_fill_rect(static_cast<int32_t>(rect.x + kGridCell - kInspectBorder),
                              static_cast<int32_t>(rect.y), kInspectBorder, kGridCell,
                              kInspectColor);
        }
    }
    if (g_extension_drag.active && g_extension_drag.bag == bag &&
        fn_item_draw_porting != nullptr) {
        void* item = module_item_locked(bag, g_extension_drag.slot);
        if (item != nullptr) {
            const int32_t ghost_x = static_cast<int32_t>(g_extension_drag.current_x - kGridCell / 2);
            const int32_t ghost_y = static_cast<int32_t>(g_extension_drag.current_y - kGridCell / 2);
            fn_item_draw_porting(item, ghost_x, ghost_y, 0, 1);
        }
    }
}

int original_bag_locked();

uint32_t* bag_size_word_locked(int bag) {
    if (g_base == 0) return nullptr;
    void*** table_slot = reinterpret_cast<void***>(g_base + G_BAG_TABLE_VMA);
    if (table_slot == nullptr || *table_slot == nullptr) return nullptr;
    if (bag < 0 || bag >= 6) return nullptr;
    void* bag_object = (*table_slot)[bag];
    if (bag_object == nullptr) return nullptr;
    return reinterpret_cast<uint32_t*>(static_cast<uint8_t*>(bag_object) + 0x10);
}

uint32_t* original_bag_size_word_locked() {
    return bag_size_word_locked(original_bag_locked());
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
                             virtual_bag::valid_index(g_virtual_bag_state.selected);
    static int last_module_view = -1;
    if (last_module_view != static_cast<int>(module_view)) {
        log_exit_trace_locked("draw_bag", 0, 0, 0);
        last_module_view = module_view ? 1 : 0;
    }
    if (module_view && g_module_view_installed) {
        original();
        refresh_projection_if_overwritten_locked();
        return;
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

void virtual_bag_draw_inven_item_wrapper() {
    const OriginalDrawInvenItemFn original =
        reinterpret_cast<OriginalDrawInvenItemFn>(g_base + fn_resolve("F_UIEQUIP_DRAW_INVEN_ITEM_VMA",
                                                                       F_UIEQUIP_DRAW_INVEN_ITEM_VMA));
    if (original == nullptr) return;
    std::lock_guard<std::mutex> lock(g_virtual_bag_mtx);
    if (g_virtual_bag_state.mode == virtual_bag::Mode::kModule &&
        virtual_bag::valid_index(g_virtual_bag_state.selected)) {
        if (g_module_view_installed) {
            // G-8 崩溃诊断（快照比对限频）：物品指针表变化时 dump 当前袋+每槽
            // 控件 data[0]——崩溃前最后一条即肇事物品。
            static void* last_items[virtual_bag::kSlotCount] = {};
            static bool last_valid = false;
            if (g_base != 0 && fn_control_object_get_child != nullptr &&
                fn_control_object_get_data != nullptr) {
                void* root = *reinterpret_cast<void**>(g_base + G_UIEQUIP_PANEL_CTRL_VMA);
                void* cur_items[virtual_bag::kSlotCount] = {};
                bool changed = !last_valid;
                for (int slot = 0; slot < virtual_bag::kSlotCount; ++slot) {
                    void* ctrl = valid_child_locked(root, slot);
                    void* data = ctrl != nullptr && fn_control_object_get_data != nullptr
                                     ? fn_control_object_get_data(ctrl)
                                     : nullptr;
                    cur_items[slot] = data != nullptr
                                          ? *reinterpret_cast<void**>(data)
                                          : nullptr;
                    if (cur_items[slot] != last_items[slot]) changed = true;
                }
                if (changed) {
                    const int cur_bag = original_bag_locked();
                    for (int slot = 0; slot < virtual_bag::kSlotCount; slot += 4) {
                        VIRTBAG_LOG(
                            "drawdiag bag=%d slot=%d..%d items=%p %p %p %p",
                            cur_bag, slot, slot + 3, cur_items[slot],
                            slot + 1 < virtual_bag::kSlotCount ? cur_items[slot + 1] : nullptr,
                            slot + 2 < virtual_bag::kSlotCount ? cur_items[slot + 2] : nullptr,
                            slot + 3 < virtual_bag::kSlotCount ? cur_items[slot + 3] : nullptr);
                    }
                    std::memcpy(last_items, cur_items, sizeof(last_items));
                    last_valid = true;
                }
            }
            original();
            return;
        }
        // Extension items occupy the original inventory rectangle without
        // entering g_inven. Mask the original bag only for this draw call so
        // the underlying ControlItem visuals do not show through the overlay.
        uint8_t** current_bag = reinterpret_cast<uint8_t**>(g_base + G_UIEQUIP_CUR_BAG_GOT_VMA);
        uint8_t saved_current = 0;
        bool masked_for_draw = false;
        if (current_bag != nullptr && *current_bag != nullptr &&
            **current_bag != kNoOriginalBagSelected) {
            saved_current = **current_bag;
            **current_bag = kNoOriginalBagSelected;
            masked_for_draw = true;
        }
        original();
        if (masked_for_draw) **current_bag = saved_current;
        return;
    }
    uint8_t** current_bag = reinterpret_cast<uint8_t**>(g_base + G_UIEQUIP_CUR_BAG_GOT_VMA);
    uint8_t saved_current = 0;
    bool restored_for_draw = false;
    if (g_virtual_bag_state.mode == virtual_bag::Mode::kExitingModule &&
        g_exit_display_bag < kNoOriginalBagSelected && current_bag != nullptr &&
        *current_bag != nullptr && **current_bag == kNoOriginalBagSelected) {
        saved_current = **current_bag;
        **current_bag = g_exit_display_bag;
        restored_for_draw = true;
    }
    original();
    if (restored_for_draw) **current_bag = saved_current;
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

int original_bag_button_index(int64_t x, int64_t y) {
    // UIEquip original bag controls are children of nested parents. The final absolute
    // rect for bag 0 is (1116, 145, 57, 57); each subsequent original bag is 70px lower.
    constexpr int64_t kOriginalBagX = 0x45c;
    constexpr int64_t kOriginalBagWidth = 0x39;
    constexpr int64_t kOriginalBagY = 0x91;
    constexpr int64_t kOriginalBagHeight = 0x39;
    constexpr int64_t kOriginalBagStepY = 0x46;
    if (x < kOriginalBagX || x >= kOriginalBagX + kOriginalBagWidth || y < kOriginalBagY) {
        return -1;
    }
    const int64_t relative_y = y - kOriginalBagY;
    const int index = static_cast<int>(relative_y / kOriginalBagStepY);
    if (index < 0 || index >= 6 || relative_y % kOriginalBagStepY >= kOriginalBagHeight) return -1;
    return index;
}

bool bind_original_exit_display_bag_locked(int bag) {
    if (g_base == 0 || bag < 0 || bag >= 6 || fn_ui_equip_refresh_item_area == nullptr) return false;
    uint8_t* direct_bag = reinterpret_cast<uint8_t*>(g_base + G_UIEQUIP_CUR_BAG_VMA);
    uint8_t** current_bag = reinterpret_cast<uint8_t**>(g_base + G_UIEQUIP_CUR_BAG_GOT_VMA);
    if (current_bag == nullptr || *current_bag == nullptr) return false;
    const uint8_t saved_direct = *direct_bag;
    const uint8_t saved_got = **current_bag;
    *direct_bag = static_cast<uint8_t>(bag);
    **current_bag = static_cast<uint8_t>(bag);
    constexpr uint32_t kCapacityMask = (1u << 25) - 1u;
    const uint32_t* size_word = bag_size_word_locked(bag);
    const bool has_capacity = size_word != nullptr && (*size_word & kCapacityMask) != 0;
    if (has_capacity) fn_ui_equip_refresh_item_area();
    *direct_bag = saved_direct;
    **current_bag = saved_got;
    return has_capacity;
}

void* module_item_locked(int bag, int slot) {
    if (!virtual_bag::valid_index(bag) || slot < 0 || slot >= virtual_bag::kSlotCount) return nullptr;
    const virtual_bag::Item& descriptor = g_virtual_bag_state.items[bag][slot];
    if (descriptor.category <= 0 || descriptor.count <= 0 || fn_create_item == nullptr) return nullptr;
    const uint32_t descriptor_hash = virtual_bag::payload_hash(descriptor);
    void* item = g_module_objects[bag][slot];
    if (item == nullptr || g_module_object_categories[bag][slot] != descriptor.category ||
        g_module_object_hashes[bag][slot] != descriptor_hash) {
        free_module_object_locked(bag, slot);
        item = materialize_module_item_locked(descriptor);
        if (item == nullptr) return nullptr;
        uint32_t handle = 0;
        if (ownership::allocate(&g_ownership_ledger, &handle) == ownership::Outcome::kOk) {
            g_module_object_handles[bag][slot] = handle;  // module-owned（G-13）
        }
        g_module_objects[bag][slot] = item;
        g_module_object_categories[bag][slot] = descriptor.category;
        g_module_object_hashes[bag][slot] = descriptor_hash;
    }
    return item;
}

// 保存门禁（P3 事故修复）：SAVE_SaveInventory 被游戏内部直调（存档面板/自动存档），
// 投影安装期间执行会把 INVEN 投影态写进存档（E-2026-08-29-02 污染事故根因）。
// 通过替换 SAVE_Save 内唯一 bl 调用点，保存前强制把投影写回快照。
typedef uint32_t (*SaveInventoryRawFn)(uint8_t* cursor);
uint32_t save_inventory_wrapper(uint8_t* cursor) {
    {
        std::lock_guard<std::mutex> lock(g_virtual_bag_mtx);
        if (g_module_view_installed) {
            VIRTBAG_LOG("save gate: restoring projected view before SAVE_SaveInventory");
            restore_module_view_locked();
        }
    }
    const uintptr_t raw = g_base != 0
                              ? g_base + fn_resolve("F_SAVE_SAVE_INVENTORY_VMA",
                                                    F_SAVE_SAVE_INVENTORY_VMA)
                              : 0;
    if (raw == 0) return 0;
    return reinterpret_cast<SaveInventoryRawFn>(raw)(cursor);
}

// G-6 终极绘制门禁：ControlItem_Draw 的 bl ITEM_DrawPorting（0xaaf28）替换——
// 投影期间任何控件 data[0] 悬空（desc 残留/重建窗口/释放时序）都不再崩游戏：
// 物品指针必须命中已知集合（模块缓存/隔离区/INVEN_pItem）才放行绘制。
bool item_pointer_known_locked(void* item);

uint32_t item_draw_porting_gate(void* item, int32_t x, int32_t y, int32_t a, int32_t b, int32_t c) {
    if (g_module_view_installed && !item_pointer_known_locked(item)) {
        VIRTBAG_LOG("draw gate: suppressed unknown item=%p", item);
        return 0;
    }
    const uintptr_t raw = g_base != 0
                              ? g_base + fn_resolve("F_ITEM_DRAW_PORTING_VMA",
                                                    F_ITEM_DRAW_PORTING_VMA)
                              : 0;
    if (raw == 0) return 0;
    typedef uint32_t (*DrawPortingFn)(void*, int32_t, int32_t, int32_t, int32_t, int32_t);
    return reinterpret_cast<DrawPortingFn>(raw)(item, x, y, a, b, c);
}

// G-6/G-7 drop 门禁 → G-8 路由：bag proc event 0x04 落袋写入（b8cc0 bl）——
// 投影期间拖动投影物品松手到这里：路由到 ext→orig 事务（真实移动），借出对象
// 不经原版 SaveItemOnEmpty 写 INVEN。非投影拖动（installed=false）直通原版。
// G-8 源识别：drop 事件的物品指针与模块缓存比对，反查扩展袋/槽。
// 事件参数自带物品指针（SaveItemOnEmpty 的 item / 0x02 的 src 控件 data），
// 无需模块维护拖动状态机——TouchHandle 全权处理拖动建立与拖影。
bool module_slot_of_item_locked(void* item, int* out_bag, int* out_slot) {
    if (item == nullptr) return false;
    for (int bag = 0; bag < virtual_bag::kBagCount; ++bag) {
        for (int slot = 0; slot < virtual_bag::kSlotCount; ++slot) {
            if (g_module_objects[bag][slot] == item) {
                *out_bag = bag;
                *out_slot = slot;
                return true;
            }
        }
    }
    return false;
}

int32_t save_item_on_empty_gate(void* item, int32_t bag) {
    {
        std::lock_guard<std::mutex> lock(g_virtual_bag_mtx);
        if (g_module_view_installed) {
            int src_bag = -1, src_slot = -1;
            if (!module_slot_of_item_locked(item, &src_bag, &src_slot)) {
                return reinterpret_cast<InvenSaveItemOnEmptyFn>(
                    g_base + fn_resolve("F_INVEN_SAVE_ITEM_ON_EMPTY_VMA",
                                        F_INVEN_SAVE_ITEM_ON_EMPTY_VMA))(item, bag);
            }
            const bool routed = bag >= 0 && bag < 5 &&
                                move_extension_to_original_locked(src_bag, src_slot, bag);
            if (routed) {
                persist_state_locked();
                refresh_projection_if_overwritten_locked();
                VIRTBAG_LOG("drop routed ext->orig bag=%d", bag);
                return 1;
            }
            VIRTBAG_LOG("drop gate: route failed bag=%d", bag);
            return 0;
        }
    }
    const uintptr_t raw = g_base != 0
                              ? g_base + fn_resolve("F_INVEN_SAVE_ITEM_ON_EMPTY_VMA",
                                                    F_INVEN_SAVE_ITEM_ON_EMPTY_VMA)
                              : 0;
    if (raw == 0) return 0;
    return reinterpret_cast<InvenSaveItemOnEmptyFn>(raw)(item, bag);
}

// 正式窗口投影（P3 方案 C，控件级）：只写控件不写 INVEN——
//   容量字（袋对象 +0x10）临时写扩展容量驱动 RefreshItemArea/DrawInvenBag 的容量语义；
//   RefreshItemArea 按容量字 SetActive/SetShow 容量外控件（隐藏）；
//   容量内控件 ControlItem_SetItem(借出对象) 显示扩展物品；
//   INVEN_pItem 全程真实（API/存档/捡取读数无污染），restore 仅写回容量字 + RefreshItemArea。
bool install_module_view_locked(int bag) {
    if (g_base == 0 || !virtual_bag::valid_index(bag)) return false;
    if (g_virtual_bag_state.capacities[bag] == 0) return false;
    if (fn_control_item_set_item == nullptr || fn_control_object_get_child == nullptr) return false;
    const int original_bag = original_bag_locked();
    if (original_bag < 0 || original_bag >= 5) {
        VIRTBAG_LOG("module view reject install: original window bag=%d (task bag reserved)",
                    original_bag);
        return false;
    }
    uint32_t* size_word = bag_size_word_locked(original_bag);
    if (size_word == nullptr) return false;

    if (g_module_view_installed) restore_module_view_locked();
    clear_original_desc_locked();  // 原版详情面板不跨视图残留

    g_original_bag_size_word = size_word;
    g_original_bag_size = *size_word;
    const int capacity = g_virtual_bag_state.capacities[bag];
    constexpr uint32_t kCapacityMask = (1u << 25) - 1u;
    *size_word = (*size_word & ~kCapacityMask) |
                 (static_cast<uint32_t>(capacity) & kCapacityMask);

    void* root = *reinterpret_cast<void**>(g_base + G_UIEQUIP_PANEL_CTRL_VMA);
    g_projected_item_root = root;
    if (root != nullptr && fn_ui_equip_refresh_item_area != nullptr) {
        fn_ui_equip_refresh_item_area();  // 按新容量禁用容量外控件 + 刷 INVEN 原版物品
        for (int slot = 0; slot < capacity; ++slot) {
            void* ctrl = valid_child_locked(root, slot);
            if (ctrl == nullptr) continue;
            void* item = module_item_locked(bag, slot);
            // 空槽也必须 SetItem(nullptr)：RefreshItemArea 刚把 INVEN 原版物品刷进控件，
            // 扩展袋空位不覆盖的话会残留原版物品显示。
            fn_control_item_set_item(ctrl, item);
            if (item != nullptr) {
                ownership::borrow_for_view(&g_ownership_ledger,
                                           g_module_object_handles[bag][slot]);
            }
        }
    }
    g_module_view_installed = true;
    g_module_view_index = bag;
    VIRTBAG_LOG("module view installed bag=%d window=%d capacity=%d", bag, original_bag, capacity);
    return true;
}

// 帧级自愈：RefreshItemArea 被游戏逻辑触发时会把控件刷回 INVEN 原版物品，
// installed 状态下每帧检测并重投影（draw wrapper 已持锁）。
void refresh_projection_if_overwritten_locked() {
    if (!g_module_view_installed || g_projected_item_root == nullptr ||
        fn_control_object_get_data == nullptr || fn_control_item_set_item == nullptr ||
        !virtual_bag::valid_index(g_module_view_index)) {
        return;
    }
    const int bag = g_module_view_index;
    const int capacity = g_virtual_bag_state.capacities[bag];
    static int heal_count = 0;
    for (int slot = 0; slot < capacity; ++slot) {
        void* item = g_module_objects[bag][slot];  // 空槽为 nullptr，同样需要清空控件
        void* ctrl = valid_child_locked(g_projected_item_root, slot);
        if (ctrl == nullptr) continue;
        void* data = fn_control_object_get_data(ctrl);
        void* current = data != nullptr ? *reinterpret_cast<void**>(data) : nullptr;
        if (current != item) {
            ++heal_count;
            fn_control_item_set_item(ctrl, item);
            VIRTBAG_LOG("projection heal #%d slot=%d current=%p item=%p",
                        heal_count, slot, current, item);
        }
    }
}

void restore_module_view_locked() {
    if (!g_module_view_installed || g_base == 0) return;
    remove_bag_info_panel_locked();
    if (g_original_bag_size_word != nullptr) {
        constexpr uint32_t kCapacityMask = (1u << 25) - 1u;
        *g_original_bag_size_word =
            (*g_original_bag_size_word & ~kCapacityMask) |
            (g_original_bag_size & kCapacityMask);
    }
    if (fn_ui_equip_refresh_item_area != nullptr) {
        fn_ui_equip_refresh_item_area();  // 容量字已还原：原版逻辑自动恢复控件（INVEN 全程真实）
    }
    g_original_bag_size_word = nullptr;
    g_original_bag_size = 0;
    g_projected_item_root = nullptr;
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
    g_exit_display_bag = kNoOriginalBagSelected;
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
    g_exit_display_bag = kNoOriginalBagSelected;
    log_exit_trace_locked("complete_cancel", 0, 0, 0);
}

void virtual_bag_f3_wrapper() {
    PopupNoArgFn original = nullptr;
    {
        std::lock_guard<std::mutex> lock(g_virtual_bag_mtx);
        ensure_state_loaded_locked();
        const bool was_exiting_module = g_virtual_bag_state.mode == virtual_bag::Mode::kExitingModule;
        if (g_module_view_installed) restore_module_view_locked();
        if (!was_exiting_module && g_virtual_bag_state.mode != virtual_bag::Mode::kOriginal) {
            virtual_bag::enter_original(&g_virtual_bag_state, original_bag_locked());
        }
        if (g_unsaved_cross_move_count > 0) {
            rollback_unsaved_cross_moves_locked();
            g_virtual_bag_state.pending = {};
            persist_state_locked();
        }
        if (was_exiting_module) {
            cancel_original_exit_locked();
        }
        g_exit_display_bag = kNoOriginalBagSelected;
        g_inventory_frame_active = false;
        g_extension_touch_capture = false;
        g_extension_drag = {};
        disable_extension_tab_buttons_locked();
        log_exit_trace_locked("f3", 0, 0, 0);
        original = g_orig_f3;
    }
    if (original != nullptr) original();
}

void virtual_bag_inventory_enter_wrapper() {
    PopupNoArgFn original = nullptr;
    {
        std::lock_guard<std::mutex> lock(g_virtual_bag_mtx);
        ensure_state_loaded_locked();
        clear_original_desc_locked();
        restore_module_view_locked();
        set_original_bag_locked(0);
        virtual_bag::enter_original(&g_virtual_bag_state, 0);
        g_inventory_frame_active = true;
        g_extension_touch_capture = false;
        g_extension_drag = {};
        disable_extension_tab_buttons_locked();
        persist_state_locked();
        original = g_orig_enter;
    }
    if (original != nullptr) original();
    if (!g_virtual_bag_enabled.load()) return;
    std::lock_guard<std::mutex> lock(g_virtual_bag_mtx);
    install_extension_tab_buttons_locked();
    if (fn_ui_equip_refresh_item_area != nullptr) fn_ui_equip_refresh_item_area();
}

void virtual_bag_save_panel_enter_wrapper() {
    if (game_in_world()) {
        virtual_bag_save_game();
    }
    if (g_orig_save_enter != nullptr) g_orig_save_enter();
}

void virtual_bag_draw_end_wrapper() {
    std::lock_guard<std::mutex> lock(g_virtual_bag_mtx);
    if (g_state != nullptr && *reinterpret_cast<uint16_t*>(g_state) != 5) {
        prepare_main_menu_locked();
        if (fn_grpx_end != nullptr) fn_grpx_end();
        return;
    }
    if (g_gamestate != nullptr && *reinterpret_cast<uint32_t*>(g_gamestate) != 0) {
        restore_module_view_locked();
        if (fn_grpx_end != nullptr) fn_grpx_end();
        return;
    }
    if (!g_inventory_frame_active) {
        restore_module_view_locked();
        set_original_bag_locked(0);
        virtual_bag::enter_original(&g_virtual_bag_state, 0);
        g_inventory_frame_active = true;
    }
    if (!g_virtual_bag_enabled.load()) {
        restore_module_view_locked();
        if (fn_grpx_end != nullptr) fn_grpx_end();
        return;
    }
    ensure_state_loaded_locked();
    commit_pending_extension_tab_locked();
    // 投影常驻（P2/P3）：module 视图有效期间保持安装；仅在状态不再指向
    // 模块视图（退出/切档/异常）时兜底恢复，防止遗留投影污染原版窗口。
    const bool module_view_expected =
        g_virtual_bag_state.mode == virtual_bag::Mode::kModule &&
        virtual_bag::valid_index(g_virtual_bag_state.selected);
    if (g_module_view_installed && !module_view_expected) restore_module_view_locked();
    if (g_module_view_installed) {
        // 拖动中物品的悬空防护：用户按住投影物品时 TouchState 记录了借出对象，
        // 延迟释放后 UIEquip_Draw 仍经 TouchState 画它（ITEM_DrawPorting 悬空崩溃）。
        // 每帧仅清 MOVING_CTRL（+0x30）——不动 press/drop 字段（避免点击失效）。
        if (g_base != 0) {
            uint8_t* touch_state = reinterpret_cast<uint8_t*>(g_base + G_TOUCH_STATE_VMA);
            *reinterpret_cast<void**>(touch_state + TOUCH_STATE_MOVING_CTRL) = nullptr;
        }
        refresh_projection_if_overwritten_locked();
    }
    if (g_module_view_installed) {
        refresh_projection_if_overwritten_locked();
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
    const bool extension_enabled = g_virtual_bag_enabled.load();
    const uint32_t gamestate = g_gamestate != nullptr ? *reinterpret_cast<uint32_t*>(g_gamestate) : 0;
    if (!extension_enabled || gamestate != 0) {
        if (event == 0x17 || event == 0x18 || event == 0x19) {
            VIRTBAG_LOG("touch bypass event=0x%llx enabled=%d gamestate=%u base=0x%llx hooked=%d",
                        static_cast<unsigned long long>(event), extension_enabled ? 1 : 0,
                        gamestate, static_cast<unsigned long long>(g_base), g_state_entry != nullptr ? 1 : 0);
        }
        return g_orig_event != nullptr ? g_orig_event(event, param, param2) : 0;
    }
    int64_t x = 0;
    int64_t y = 0;
    bool exiting_to_original = false;
    if (event == 0x17 && param != 0) {
        x = *reinterpret_cast<const int64_t*>(param);
        y = *reinterpret_cast<const int64_t*>(param + 8);
        std::lock_guard<std::mutex> lock(g_virtual_bag_mtx);
        ensure_state_loaded_locked();
        log_exit_trace_locked("event_enter", event, param, param2, x, y);
        const int64_t grid_width = 4 * kGridStep - (kGridStep - kGridCell);
        const int64_t grid_height = 4 * kGridStep - (kGridStep - kGridCell);
        VIRTBAG_LOG("touch bounds event=0x%llx x=%lld y=%lld grid=%lld,%lld %lldx%lld grid_hit=%d tab_origin=%lld,%lld mode=%d selected=%d overlay=%d root=%p root_gen=%llu state_gen=%llu",
                    static_cast<unsigned long long>(event), static_cast<long long>(x), static_cast<long long>(y),
                    static_cast<long long>(kGridX), static_cast<long long>(kGridY),
                    static_cast<long long>(grid_width), static_cast<long long>(grid_height),
                    extension_grid_hit(x, y) ? 1 : 0, static_cast<long long>(kCellX),
                    static_cast<long long>(kCellY), static_cast<int>(g_virtual_bag_state.mode),
                    g_virtual_bag_state.selected, g_module_view_installed ? 1 : 0,
                    g_extension_tab_root, static_cast<unsigned long long>(g_extension_tab_generation),
                    static_cast<unsigned long long>(g_inventory_generation));
        if (g_extension_touch_capture) return 1;
        if (extension_grid_hit(x, y)) {
            // G-6/G-7 零干预原则：投影常驻时网格触摸完全放行原版控件链
            // （TouchHandle 自带命中/选中动画/详情；操作按钮由 menu_gate 拦截；
            // 拖动 drop 由 SaveItemOnEmpty 门禁拦截）。模块不做任何状态清理——
            // press 时 reset/clear 会破坏 TouchHandle 的按压记录导致点击失效。
        }
        if (g_virtual_bag_state.mode != virtual_bag::Mode::kOriginal &&
            grid_slot_index(x, y, kOriginalGridX, kOriginalGridY) >= 0) {
            // The original grid is hidden underneath the module grid layout.
            // Consume it even when no extension drag is active.
            g_extension_drag = {};
            reset_drag_state_locked(nullptr);
            g_extension_touch_capture = true;
            return 1;
        }
        const int target_original_bag = original_bag_button_index(x, y);
        if (!g_extension_touch_capture && g_virtual_bag_state.mode == virtual_bag::Mode::kModule &&
            target_original_bag >= 0) {
            restore_module_view_locked();
            virtual_bag::begin_exit_module(&g_virtual_bag_state);
            clear_original_bag_selection_locked();
            g_exit_display_bag = g_original_current_got < kNoOriginalBagSelected ?
                                 g_original_current_got : g_original_current_direct;
            if (bind_original_exit_display_bag_locked(target_original_bag)) {
                g_exit_display_bag = static_cast<uint8_t>(target_original_bag);
            }
            exiting_to_original = true;
            log_exit_trace_locked("original_press_pre_orig", event, param, param2, x, y);
        }
    }
    if (event == 0x18) {
        if (param != 0) {
            x = *reinterpret_cast<const int64_t*>(param);
            y = *reinterpret_cast<const int64_t*>(param + 8);
        } else if (g_base != 0) {
            const uint8_t* touch_state = reinterpret_cast<const uint8_t*>(g_base + G_TOUCH_STATE_VMA);
            x = *reinterpret_cast<const int64_t*>(touch_state + 0x60);
            y = *reinterpret_cast<const int64_t*>(touch_state + 0x68);
        }
        std::lock_guard<std::mutex> lock(g_virtual_bag_mtx);
        ensure_state_loaded_locked();
        log_exit_trace_locked("event_release", event, param, param2, x, y);
        if (g_extension_touch_capture) {
            if (param == 0 && x == 0 && y == 0 && g_extension_drag.active) {
                x = g_extension_drag.current_x;
                y = g_extension_drag.current_y;
            }
            if (param == 0 && x == 0 && y == 0) {
                g_extension_drag = {};
                g_extension_touch_capture = false;
                reset_drag_state_locked(nullptr);
                return 1;
            }
            constexpr int64_t kTouchSlop = 24;
            const int64_t dx = x - g_extension_drag.press_x;
            const int64_t dy = y - g_extension_drag.press_y;
            const bool is_click = g_extension_drag.active &&
                                  dx >= -kTouchSlop && dx <= kTouchSlop &&
                                  dy >= -kTouchSlop && dy <= kTouchSlop;
            if (is_click) {
                g_virtual_bag_state.inspected = static_cast<int>(g_extension_drag.slot);
                const virtual_bag::Item& item =
                    g_virtual_bag_state.items[g_extension_drag.bag][g_extension_drag.slot];
                VIRTBAG_LOG("extension item click bag=%d slot=%d category=%d count=%d",
                            g_extension_drag.bag, g_extension_drag.slot, item.category,
                            item.count);
                log_exit_trace_locked("extension_item_click", event, param, param2, x, y);
            } else {
                const bool handled = g_extension_drag.active &&
                                     handle_bag_drop_release_locked(x, y);
                (void)handled;
            }
            reset_drag_state_locked(nullptr);
            g_extension_drag = {};
            g_extension_touch_capture = false;
            return 1;
        }
        if (g_virtual_bag_state.mode == virtual_bag::Mode::kOriginal &&
            handle_bag_drop_release_locked(x, y)) {
            return 1;
        }
        // G-10：tab 点击的坐标兜底已删除——tab 点击由控件回调
        // （extension_tab_button_clicked → queue_extension_tab_click）唯一处理，
        // 消除坐标常量与控件树的双路径分歧。
        // Phase one is deliberately read-only. Do not route release events to
        // the legacy projection/move transaction handlers.
    }
    if (event == 0x19) {
        if (param != 0) {
            x = *reinterpret_cast<const int64_t*>(param);
            y = *reinterpret_cast<const int64_t*>(param + 8);
        } else if (g_base != 0) {
            const uint8_t* touch_state = reinterpret_cast<const uint8_t*>(g_base + G_TOUCH_STATE_VMA);
            x = *reinterpret_cast<const int64_t*>(touch_state + 0x60);
            y = *reinterpret_cast<const int64_t*>(touch_state + 0x68);
        }
        std::lock_guard<std::mutex> lock(g_virtual_bag_mtx);
        ensure_state_loaded_locked();
        if (g_extension_touch_capture) {
            // 0x19 is the native MOVE event. Keep the source and capture
            // alive until 0x18 performs click-vs-drop classification.
            if (g_extension_drag.active) {
                g_extension_drag.current_x = x;
                g_extension_drag.current_y = y;
            }
            return 1;
        }
    }
    if (g_extension_touch_capture && event != 0x17 && event != 0x18 && event != 0x19) {
        std::lock_guard<std::mutex> lock(g_virtual_bag_mtx);
        g_extension_drag = {};
        g_extension_touch_capture = false;
        reset_drag_state_locked(nullptr);
        log_exit_trace_locked("event_cancel", event, param, param2, x, y);
        return 1;
    }
    if (g_extension_touch_capture) {
        // Consume every event in the captured sequence, not only release and cancel.
        return 1;
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
            g_exit_display_bag = kNoOriginalBagSelected;
        } else if (event == 0x17 || event == 0x18) {
            g_virtual_bag_state.original_selected = original_bag_locked();
        }
    }
    return result;
}

bool inject_locked() {
    std::lock_guard<std::mutex> lock(g_virtual_bag_mtx);
    if (!g_virtual_bag_enabled.load()) return false;
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
    g_orig_enter = reinterpret_cast<PopupNoArgFn>(*reinterpret_cast<uintptr_t*>(entry + 0x10));

    const uintptr_t equip_draw = g_base + fn_resolve("F_UIEQUIP_DRAW_VMA", F_UIEQUIP_DRAW_VMA);
    const uintptr_t item_draw_call = equip_draw + 0x94;
    constexpr uint32_t kOriginalItemDrawCall = 0x97fffe33;
    constexpr size_t kPageSize = 4096;
    if (g_item_draw_patch_addr == 0) {
        const uintptr_t wrapper = reinterpret_cast<uintptr_t>(&virtual_bag_draw_inven_item_wrapper);
        const int64_t direct_delta = static_cast<int64_t>(wrapper) - static_cast<int64_t>(item_draw_call);
        uintptr_t branch_target = wrapper;
        if ((direct_delta & 0x3) != 0 || direct_delta <= -0x08000000LL || direct_delta >= 0x08000000LL) {
            g_item_draw_thunk = allocate_draw_thunk(item_draw_call, wrapper);
            if (g_item_draw_thunk == nullptr) {
                VIRTBAG_LOG("inventory item draw thunk allocation failed");
                return false;
            }
            branch_target = reinterpret_cast<uintptr_t>(g_item_draw_thunk);
        }
        const int64_t delta = static_cast<int64_t>(branch_target) - static_cast<int64_t>(item_draw_call);
        if ((delta & 0x3) != 0 || delta <= -0x08000000LL || delta >= 0x08000000LL) {
            VIRTBAG_LOG("inventory item draw target out of range");
            return false;
        }
        const uint32_t replacement = 0x94000000u | (static_cast<uint32_t>(delta >> 2) & 0x03ffffffu);
        const uintptr_t page = item_draw_call & ~(static_cast<uintptr_t>(kPageSize) - 1);
        if (mprotect(reinterpret_cast<void*>(page), kPageSize, PROT_READ | PROT_WRITE | PROT_EXEC) != 0) {
            VIRTBAG_LOG("inventory item draw patch mprotect failed errno=%d", errno);
            return false;
        }
        const uint32_t current = *reinterpret_cast<uint32_t*>(item_draw_call);
        if (current != kOriginalItemDrawCall && current != replacement) {
            VIRTBAG_LOG("inventory item draw patch mismatch got=0x%08x", current);
            return false;
        }
        *reinterpret_cast<uint32_t*>(item_draw_call) = replacement;
        __builtin___clear_cache(reinterpret_cast<char*>(item_draw_call),
                                 reinterpret_cast<char*>(item_draw_call + sizeof(uint32_t)));
        g_item_draw_patch_addr = item_draw_call;
        VIRTBAG_LOG("inventory item draw hook patched replacement=0x%08x", replacement);
    }

    const uintptr_t bag_draw_call = equip_draw + 0x98;
    constexpr uint32_t kOriginalBagDrawCall = 0x97fffee8;
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

    if (g_save_inventory_patch_addr == 0) {
        const uintptr_t call_addr =
            g_base + fn_resolve("F_SAVE_SAVE_INVENTORY_CALLSITE_VMA",
                                F_SAVE_SAVE_INVENTORY_CALLSITE_VMA);
        constexpr uint32_t kOriginalSaveCall = 0x97fff987;
        const uintptr_t wrapper = reinterpret_cast<uintptr_t>(&save_inventory_wrapper);
        const int64_t direct_delta = static_cast<int64_t>(wrapper) - static_cast<int64_t>(call_addr);
        uintptr_t branch_target = wrapper;
        if ((direct_delta & 0x3) != 0 || direct_delta <= -0x08000000LL || direct_delta >= 0x08000000LL) {
            g_save_inventory_thunk = allocate_draw_thunk(call_addr, wrapper);
            if (g_save_inventory_thunk == nullptr) {
                VIRTBAG_LOG("save gate thunk allocation failed call=%p wrapper=%p",
                            reinterpret_cast<void*>(call_addr), wrapper);
                return false;
            }
            branch_target = reinterpret_cast<uintptr_t>(g_save_inventory_thunk);
        }
        const int64_t delta = static_cast<int64_t>(branch_target) - static_cast<int64_t>(call_addr);
        if ((delta & 0x3) != 0 || delta <= -0x08000000LL || delta >= 0x08000000LL) {
            VIRTBAG_LOG("save gate branch target out of range call=%p target=%p",
                        reinterpret_cast<void*>(call_addr), branch_target);
            return false;
        }
        const uint32_t replacement = 0x94000000u | (static_cast<uint32_t>(delta >> 2) & 0x03ffffffu);
        const uintptr_t page = call_addr & ~(static_cast<uintptr_t>(kPageSize) - 1);
        if (mprotect(reinterpret_cast<void*>(page), kPageSize, PROT_READ | PROT_WRITE | PROT_EXEC) != 0) {
            VIRTBAG_LOG("save gate patch mprotect failed errno=%d", errno);
            return false;
        }
        const uint32_t current = *reinterpret_cast<uint32_t*>(call_addr);
        if (current != kOriginalSaveCall && current != replacement) {
            VIRTBAG_LOG("save gate patch mismatch got=0x%08x", current);
            return false;
        }
        *reinterpret_cast<uint32_t*>(call_addr) = replacement;
        __builtin___clear_cache(reinterpret_cast<char*>(call_addr),
                                reinterpret_cast<char*>(call_addr + sizeof(uint32_t)));
        g_save_inventory_patch_addr = call_addr;
        VIRTBAG_LOG("save gate hook patched call=%p replacement=0x%08x",
                    reinterpret_cast<void*>(call_addr), replacement);
    }

    if (g_drop_gate_patch_addr == 0) {
        const uintptr_t call_addr = g_base + 0xb8cc0;
        constexpr uint32_t kOriginalDropCall = 0x94012fc8;
        const uintptr_t wrapper = reinterpret_cast<uintptr_t>(&save_item_on_empty_gate);
        const int64_t direct_delta = static_cast<int64_t>(wrapper) - static_cast<int64_t>(call_addr);
        uintptr_t branch_target = wrapper;
        if ((direct_delta & 0x3) != 0 || direct_delta <= -0x08000000LL || direct_delta >= 0x08000000LL) {
            g_drop_gate_thunk = allocate_draw_thunk(call_addr, wrapper);
            if (g_drop_gate_thunk == nullptr) {
                VIRTBAG_LOG("drop gate thunk allocation failed call=%p wrapper=%p",
                            reinterpret_cast<void*>(call_addr), wrapper);
                return false;
            }
            branch_target = reinterpret_cast<uintptr_t>(g_drop_gate_thunk);
        }
        const int64_t delta = static_cast<int64_t>(branch_target) - static_cast<int64_t>(call_addr);
        if ((delta & 0x3) != 0 || delta <= -0x08000000LL || delta >= 0x08000000LL) {
            VIRTBAG_LOG("drop gate branch target out of range call=%p target=%p",
                        reinterpret_cast<void*>(call_addr), branch_target);
            return false;
        }
        const uint32_t replacement = 0x94000000u | (static_cast<uint32_t>(delta >> 2) & 0x03ffffffu);
        const uintptr_t page = call_addr & ~(static_cast<uintptr_t>(kPageSize) - 1);
        if (mprotect(reinterpret_cast<void*>(page), kPageSize, PROT_READ | PROT_WRITE | PROT_EXEC) != 0) {
            VIRTBAG_LOG("drop gate patch mprotect failed errno=%d", errno);
            return false;
        }
        const uint32_t current = *reinterpret_cast<uint32_t*>(call_addr);
        if (current != kOriginalDropCall && current != replacement) {
            VIRTBAG_LOG("drop gate patch mismatch got=0x%08x", current);
            return false;
        }
        *reinterpret_cast<uint32_t*>(call_addr) = replacement;
        __builtin___clear_cache(reinterpret_cast<char*>(call_addr),
                                reinterpret_cast<char*>(call_addr + sizeof(uint32_t)));
        g_drop_gate_patch_addr = call_addr;
        VIRTBAG_LOG("drop gate hook patched call=%p replacement=0x%08x",
                    reinterpret_cast<void*>(call_addr), replacement);
    }

    if (g_draw_gate_patch_addr == 0) {
        const uintptr_t call_addr = g_base + 0xaaf28;
        constexpr uint32_t kOriginalDrawCall = 0x94016d49;
        const uintptr_t wrapper = reinterpret_cast<uintptr_t>(&item_draw_porting_gate);
        const int64_t direct_delta = static_cast<int64_t>(wrapper) - static_cast<int64_t>(call_addr);
        uintptr_t branch_target = wrapper;
        if ((direct_delta & 0x3) != 0 || direct_delta <= -0x08000000LL || direct_delta >= 0x08000000LL) {
            g_draw_gate_thunk = allocate_draw_thunk(call_addr, wrapper);
            if (g_draw_gate_thunk == nullptr) {
                VIRTBAG_LOG("draw gate thunk allocation failed call=%p wrapper=%p",
                            reinterpret_cast<void*>(call_addr), wrapper);
                return false;
            }
            branch_target = reinterpret_cast<uintptr_t>(g_draw_gate_thunk);
        }
        const int64_t delta = static_cast<int64_t>(branch_target) - static_cast<int64_t>(call_addr);
        if ((delta & 0x3) != 0 || delta <= -0x08000000LL || delta >= 0x08000000LL) {
            VIRTBAG_LOG("draw gate branch target out of range call=%p target=%p",
                        reinterpret_cast<void*>(call_addr), branch_target);
            return false;
        }
        const uint32_t replacement = 0x94000000u | (static_cast<uint32_t>(delta >> 2) & 0x03ffffffu);
        const uintptr_t page = call_addr & ~(static_cast<uintptr_t>(kPageSize) - 1);
        if (mprotect(reinterpret_cast<void*>(page), kPageSize, PROT_READ | PROT_WRITE | PROT_EXEC) != 0) {
            VIRTBAG_LOG("draw gate patch mprotect failed errno=%d", errno);
            return false;
        }
        const uint32_t current = *reinterpret_cast<uint32_t*>(call_addr);
        if (current != kOriginalDrawCall && current != replacement) {
            VIRTBAG_LOG("draw gate patch mismatch got=0x%08x", current);
            return false;
        }
        *reinterpret_cast<uint32_t*>(call_addr) = replacement;
        __builtin___clear_cache(reinterpret_cast<char*>(call_addr),
                                reinterpret_cast<char*>(call_addr + sizeof(uint32_t)));
        g_draw_gate_patch_addr = call_addr;
        VIRTBAG_LOG("draw gate hook patched call=%p replacement=0x%08x",
                    reinterpret_cast<void*>(call_addr), replacement);
    }
    *reinterpret_cast<uintptr_t*>(entry + 0x28) = reinterpret_cast<uintptr_t>(&virtual_bag_f3_wrapper);
    *reinterpret_cast<uintptr_t*>(entry + 0x38) = reinterpret_cast<uintptr_t>(&virtual_bag_event);
    *reinterpret_cast<uintptr_t*>(entry + 0x10) = reinterpret_cast<uintptr_t>(&virtual_bag_inventory_enter_wrapper);
    uint8_t* save_entry = find_popup_state_entry(
        g_base + fn_resolve("F_PANEL_SAVE_SLOT_ENTER", F_PANEL_SAVE_SLOT_ENTER));
    if (save_entry != nullptr && save_entry != entry) {
        g_save_state_entry = save_entry;
        g_orig_save_enter = reinterpret_cast<PopupNoArgFn>(*reinterpret_cast<uintptr_t*>(save_entry + 0x10));
        *reinterpret_cast<uintptr_t*>(save_entry + 0x10) =
            reinterpret_cast<uintptr_t>(&virtual_bag_save_panel_enter_wrapper);
    }
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

void ensure_lifecycle_thread() {
    if (g_lifecycle_thread_started.exchange(true)) return;
    std::thread([]() {
        int last_state = -1;
        for (;;) {
            if (g_state != nullptr) {
                const int state = static_cast<int>(*reinterpret_cast<uint16_t*>(g_state));
                if (last_state == 5 && state != 5) {
                    std::lock_guard<std::mutex> lock(g_virtual_bag_mtx);
                    prepare_main_menu_locked();
                }
                last_state = state;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(150));
        }
    }).detach();
}

void prepare_main_menu_locked() {
    if (g_module_view_installed) restore_module_view_locked();
    rollback_unsaved_cross_moves_locked();
    clear_module_cache_locked();
    g_virtual_bag_state = {};
    g_loaded_slot = -2;
    g_item_state_dirty = false;
    g_inventory_frame_active = false;
    disable_extension_tab_buttons_locked();
    g_extension_tab_buttons.fill(nullptr);
}

}  // namespace

// G-8：item proc 0x02（drop 到扩展格控件）路由——src=模块拖动状态（press 时
// projection_slot_at 记录），dst=落点控件索引。成功返回 true（wrapper 吞原版事件）。
bool virtual_bag_projection_drop_to_slot(void* dst_control, void* src_control) {
    std::lock_guard<std::mutex> lock(g_virtual_bag_mtx);
    if (!g_module_view_installed || dst_control == nullptr ||
        src_control == nullptr || fn_ui_equip_get_item_slot_index == nullptr ||
        fn_control_object_get_data == nullptr) {
        return false;
    }
    void* src_data = fn_control_object_get_data(src_control);
    void* item = src_data != nullptr ? *reinterpret_cast<void**>(src_data) : nullptr;
    int src_bag = -1, src_slot = -1;
    if (item == nullptr || !module_slot_of_item_locked(item, &src_bag, &src_slot)) {
        return false;
    }
    const int dst_slot = fn_ui_equip_get_item_slot_index(dst_control);
    if (dst_slot < 0 || dst_slot >= g_virtual_bag_state.capacities[src_bag]) {
        return false;
    }
    const bool routed = move_extension_to_extension_locked(src_bag, src_slot, src_bag, dst_slot);
    if (routed) {
        persist_state_locked();
        refresh_projection_if_overwritten_locked();
        VIRTBAG_LOG("drop routed ext->ext dst_slot=%d", dst_slot);
    }
    return routed;
}

void virtual_bag_ui_start_auto_inject() {
    ensure_inject_thread();
    ensure_lifecycle_thread();
}

bool virtual_bag_module_view_installed() {
    std::lock_guard<std::mutex> lock(g_virtual_bag_mtx);
    return g_module_view_installed;
}

bool virtual_bag_original_item_input_blocked() {
    std::lock_guard<std::mutex> lock(g_virtual_bag_mtx);
    const bool blocked = g_virtual_bag_enabled.load() &&
                         (g_virtual_bag_state.mode == virtual_bag::Mode::kModule ||
                          g_virtual_bag_state.mode == virtual_bag::Mode::kExitingModule);
    if (blocked) {
        VIRTBAG_LOG("original item input blocked mode=%d selected=%d",
                    static_cast<int>(g_virtual_bag_state.mode), g_virtual_bag_state.selected);
    }
    return blocked;
}

bool set_virtual_bag_enabled(bool enabled) {
    {
        std::lock_guard<std::mutex> lock(g_virtual_bag_mtx);
        g_virtual_bag_enabled.store(enabled);
        if (!enabled) {
            restore_module_view_locked();
            g_virtual_bag_state.mode = virtual_bag::Mode::kOriginal;
            g_virtual_bag_state.selected = -1;
            g_virtual_bag_state.inspected = -1;
        }
    }
    if (enabled) virtual_bag_ui_start_auto_inject();
    return true;
}

bool virtual_bag_enabled() {
    return g_virtual_bag_enabled.load();
}

void virtual_bag_prepare_save_slot_load() {
    std::lock_guard<std::mutex> lock(g_virtual_bag_mtx);
    if (g_module_view_installed) restore_module_view_locked();
    rollback_unsaved_cross_moves_locked();
    clear_module_cache_locked();
    g_virtual_bag_state = {};
    g_loaded_slot = -2;
    disable_extension_tab_buttons_locked();
    g_extension_tab_buttons.fill(nullptr);
}

void virtual_bag_prepare_main_menu() {
    std::lock_guard<std::mutex> lock(g_virtual_bag_mtx);
    prepare_main_menu_locked();
}

bool virtual_bag_save_game() {
    {
        std::lock_guard<std::mutex> lock(g_virtual_bag_mtx);
        ensure_state_loaded_locked();
        if (g_module_view_installed) restore_module_view_locked();
        recover_pending_transaction_locked();
    }
    // fn_save 必须在锁外：内部 SAVE_SaveInventory 门禁 wrapper 需重新获锁做投影恢复，
    // 同线程重入 std::mutex 会自死锁（与 op_ok 锁内刷新同型）。
    g_explicit_save_in_progress = true;
    const int result = fn_save != nullptr ? fn_save() : 0;
    bool state_saved = false;
    {
        std::lock_guard<std::mutex> lock(g_virtual_bag_mtx);
        if (result != 0) {
            state_saved = persist_state_locked(true);
            if (state_saved) {
                g_unsaved_cross_move_count = 0;
                g_virtual_bag_state.pending = {};
                g_item_state_dirty = false;
            }
        }
    }
    g_explicit_save_in_progress = false;
    VIRTBAG_LOG("virtual bag save result native=%d sidecar=%d unsaved_cross=%zu pending=%d",
                result != 0 ? 1 : 0, state_saved ? 1 : 0, g_unsaved_cross_move_count,
                g_virtual_bag_state.pending.valid ? 1 : 0);
    return state_saved && result != 0;
}

// G-14（方案 C）：原版物品操作后 RefreshItemArea 会以 INVEN 重刷控件，窗口袋投影需重写。
// 投影只占窗口袋；其他袋调用为 no-op。调用方（data_op_*）不持模块锁，此处安全获锁。
bool virtual_bag_sync_projected_slot(int display_bag, int slot) {
    std::lock_guard<std::mutex> lock(g_virtual_bag_mtx);
    if (!g_module_view_installed || !virtual_bag::valid_index(g_module_view_index) ||
        g_projected_item_root == nullptr || fn_control_object_get_child == nullptr ||
        fn_control_item_set_item == nullptr) {
        return false;
    }
    if (display_bag < 0 || display_bag >= 5 || display_bag != original_bag_locked()) {
        return false;
    }
    if (slot < 0 || slot >= g_virtual_bag_state.capacities[g_module_view_index]) {
        return false;
    }
    void* live_root = g_base != 0 ? *reinterpret_cast<void**>(g_base + G_UIEQUIP_PANEL_CTRL_VMA)
                                  : nullptr;
    void* ctrl = valid_child_locked(live_root, slot);
    void* item = g_module_objects[g_module_view_index][slot];
    if (ctrl == nullptr) return false;
    fn_control_item_set_item(ctrl, item);
    return true;
}

bool virtual_bag_sync_projected_item_control(void* control) {
    if (control == nullptr) return false;
    std::lock_guard<std::mutex> lock(g_virtual_bag_mtx);
    if (!g_module_view_installed || !virtual_bag::valid_index(g_module_view_index) ||
        g_projected_item_root == nullptr || fn_control_object_get_child == nullptr ||
        fn_control_item_set_item == nullptr) {
        return false;
    }
    const int capacity = g_virtual_bag_state.capacities[g_module_view_index];
    for (int slot = 0; slot < capacity; ++slot) {
        if (fn_control_object_get_child(g_projected_item_root, slot) == control) {
            fn_control_item_set_item(control, g_module_objects[g_module_view_index][slot]);
            return true;
        }
    }
    return false;
}

bool virtual_bag_sync_projected_bag() {
    std::lock_guard<std::mutex> lock(g_virtual_bag_mtx);
    refresh_projection_if_overwritten_locked();
    return g_module_view_installed;
}

void virtual_bag_ui_register_bridge(JNIEnv* env, jclass bridge_class) {
    if (env == nullptr || bridge_class == nullptr) return;
    std::lock_guard<std::mutex> lock(g_virtual_bag_mtx);
    if (g_virtual_bag_bridge_class != nullptr) env->DeleteGlobalRef(g_virtual_bag_bridge_class);
    g_virtual_bag_bridge_class = static_cast<jclass>(env->NewGlobalRef(bridge_class));
}

std::string data_virtual_bag_ui_status_json() {
    std::lock_guard<std::mutex> lock(g_virtual_bag_mtx);
    if (!game_in_world()) {
        return "{\"injected\":false,\"state\":{}}";
    }
    ensure_state_loaded_locked();
    return "{\"injected\":" + std::string(g_state_entry != nullptr ? "true" : "false") +
           ",\"extension_tab_button\":" +
           std::string(g_extension_tab_buttons[0] != nullptr ? "true" : "false") +
           ",\"state\":" + virtual_bag::state_json(g_virtual_bag_state) + "}";
}

std::string data_virtual_bag_test_equip(int index, int bag_type) {
    bool updated = false;
    {
        std::lock_guard<std::mutex> lock(g_virtual_bag_mtx);
        ensure_state_loaded_locked();
        updated = virtual_bag::set_test_equipped(&g_virtual_bag_state, index, bag_type);
        if (updated) g_item_state_dirty = true;
    }
    if (!updated) return op_err("bad virtual bag index or bag type");
    return op_ok();
}

std::string data_virtual_bag_test_item(int index, int slot, int category, int count) {
    bool updated = false;
    {
        std::lock_guard<std::mutex> lock(g_virtual_bag_mtx);
        ensure_state_loaded_locked();
        updated = virtual_bag::set_item(&g_virtual_bag_state, index, slot, category, count);
        if (updated) g_item_state_dirty = true;
    }
    if (!updated) return op_err("bad virtual bag item");
    return op_ok();
}

namespace {

const char* extension_recovery_action_name_locked() {
    const virtual_bag::RecoveryAction action =
        virtual_bag::recovery_action(g_virtual_bag_state, g_virtual_bag_state.pending);
    if (action == virtual_bag::RecoveryAction::kComplete) return "complete";
    if (action == virtual_bag::RecoveryAction::kRollback) return "rollback";
    return "none";
}

std::string extension_bag_status_json_locked() {
    return "{\"enabled\":" + std::string(g_virtual_bag_enabled.load() ? "true" : "false") +
           ",\"injected\":" + std::string(g_state_entry != nullptr ? "true" : "false") +
           ",\"extension_tab_button\":" +
           std::string(g_extension_tab_buttons[0] != nullptr ? "true" : "false") +
           ",\"inventory_frame_active\":" +
           std::string(g_inventory_frame_active ? "true" : "false") +
           ",\"recovery_action\":\"" + extension_recovery_action_name_locked() + "\"" +
           ",\"state\":" + virtual_bag::state_json(g_virtual_bag_state) + "}";
}

std::string extension_bag_not_ready_error_locked() {
    if (!g_virtual_bag_enabled.load()) return op_err("extension bag disabled");
    return op_err("not in game");
}

bool extension_bag_ready_locked() {
    return g_virtual_bag_enabled.load() && game_in_world();
}

int extension_internal_bag(int logical_bag) {
    if (logical_bag < 6 || logical_bag >= 6 + virtual_bag::kBagCount) return -1;
    return logical_bag - 6;
}

std::string extension_bag_view_result_json(bool ok, const char* error) {
    if (!ok) return op_err(error);
    return "{\"ok\":true,\"state\":" + extension_bag_status_json_locked() + "}";
}

}  // namespace

std::string data_op_extension_bag_status_json() {
    std::lock_guard<std::mutex> lock(g_virtual_bag_mtx);
    if (!game_in_world()) {
        return "{\"enabled\":" + std::string(g_virtual_bag_enabled.load() ? "true" : "false") +
               ",\"injected\":false,\"extension_tab_button\":false," +
               "\"inventory_frame_active\":false,\"recovery_action\":\"none\",\"state\":{}}";
    }
    ensure_state_loaded_locked();
    return extension_bag_status_json_locked();
}

std::string data_op_extension_bag_enter_view(int logical_bag) {
    if (!extension_bag_ready_locked()) return extension_bag_not_ready_error_locked();
    const int internal_bag = extension_internal_bag(logical_bag);
    if (internal_bag < 0) return op_err("bad extension bag (6-10)");
    std::lock_guard<std::mutex> lock(g_virtual_bag_mtx);
    ensure_state_loaded_locked();
    // 面板门禁（P3 真机实证）：控件树仅在背包面板打开时有效，面板关闭时
    // enter_view 会投影到 stale 树（点击失效/重建后被 RefreshItemArea 冲掉）。
    if (!g_inventory_frame_active) return op_err("inventory panel not open");
    if (g_virtual_bag_state.mode == virtual_bag::Mode::kModule) {
        return op_err("already in extension view");
    }
    if (g_virtual_bag_state.mode == virtual_bag::Mode::kExitingModule) {
        return op_err("exit in progress");
    }
    handle_extension_tab_click_locked(internal_bag);
    const bool entered = g_virtual_bag_state.mode == virtual_bag::Mode::kModule;
    return extension_bag_view_result_json(entered, "enter extension view failed");
}

std::string data_op_extension_bag_exit_view() {
    if (!extension_bag_ready_locked()) return extension_bag_not_ready_error_locked();
    std::lock_guard<std::mutex> lock(g_virtual_bag_mtx);
    ensure_state_loaded_locked();
    if (g_virtual_bag_state.mode == virtual_bag::Mode::kExitingModule) {
        return op_err("exit in progress");
    }
    if (g_virtual_bag_state.mode != virtual_bag::Mode::kModule || g_virtual_bag_state.selected < 0) {
        return op_err("not in extension view");
    }
    handle_extension_tab_click_locked(g_virtual_bag_state.selected);
    const bool exited = g_virtual_bag_state.mode == virtual_bag::Mode::kOriginal;
    return extension_bag_view_result_json(exited, "exit extension view failed");
}

std::string data_op_extension_bag_select_bag(int logical_bag) {
    if (!extension_bag_ready_locked()) return extension_bag_not_ready_error_locked();
    const int internal_bag = extension_internal_bag(logical_bag);
    if (internal_bag < 0) return op_err("bad extension bag (6-10)");
    std::lock_guard<std::mutex> lock(g_virtual_bag_mtx);
    ensure_state_loaded_locked();
    if (g_virtual_bag_state.mode != virtual_bag::Mode::kModule) {
        return op_err("not in extension view");
    }
    if (g_virtual_bag_state.selected == internal_bag) return op_err("bag already selected");
    handle_extension_tab_click_locked(internal_bag);
    const bool selected = g_virtual_bag_state.selected == internal_bag;
    return extension_bag_view_result_json(selected, "select bag failed");
}

std::string data_op_extension_bag_click_item(int logical_bag, int slot) {
    if (!extension_bag_ready_locked()) return extension_bag_not_ready_error_locked();
    if (slot < 0 || slot >= virtual_bag::kSlotCount) return op_err("bad slot");
    const int internal_bag = extension_internal_bag(logical_bag);
    if (internal_bag < 0) return op_err("bad extension bag (6-10)");
    std::lock_guard<std::mutex> lock(g_virtual_bag_mtx);
    ensure_state_loaded_locked();
    if (g_virtual_bag_state.mode != virtual_bag::Mode::kModule) {
        return op_err("not in extension view");
    }
    if (g_virtual_bag_state.selected != internal_bag) return op_err("bag not selected");
    if (slot >= g_virtual_bag_state.capacities[internal_bag]) {
        return op_err("slot beyond derived capacity");
    }
    const virtual_bag::Item& item = g_virtual_bag_state.items[internal_bag][slot];
    if (item.category <= 0 || item.count <= 0) {
        g_virtual_bag_state.inspected = -1;
        persist_state_locked();
        return "{\"ok\":true,\"item\":null}";
    }
    g_virtual_bag_state.inspected = slot;
    persist_state_locked();
    return "{\"ok\":true,\"item\":{\"bag\":" + std::to_string(logical_bag) +
           ",\"slot\":" + std::to_string(slot) +
           ",\"category\":" + std::to_string(item.category) +
           ",\"count\":" + std::to_string(item.count) +
           ",\"payload\":\"" +
           virtual_bag::base64_encode(item.payload.data(), item.payload_size) + "\"}}";
}

// P3 袋解除（原版语义）：有物品拒绝（弹窗拒绝对应此错误），空袋解除=装备清零+容量归 0。
// 解除后的背包物品回流背包空位，等 P4 对象桥接（当前装备态为测试标记，无真实对象）。
std::string data_op_extension_bag_unequip(int logical_bag) {
    if (!extension_bag_ready_locked()) return extension_bag_not_ready_error_locked();
    const int internal_bag = extension_internal_bag(logical_bag);
    if (internal_bag < 0) return op_err("bad extension bag (6-10)");
    std::lock_guard<std::mutex> lock(g_virtual_bag_mtx);
    ensure_state_loaded_locked();
    if (g_virtual_bag_state.capacities[internal_bag] == 0) return op_err("bag not equipped");
    if (!virtual_bag::unequip_bag(&g_virtual_bag_state, internal_bag)) {
        return op_err("bag not empty");
    }
    if (g_module_view_installed && g_module_view_index == internal_bag) {
        restore_module_view_locked();
    }
    g_item_state_dirty = true;
    if (persist_state_locked()) return op_ok();
    return op_err("persist failed");
}

std::string data_op_extension_bag_move_item(int from_bag, int from_slot, int to_bag, int to_slot) {
    if (!extension_bag_ready_locked()) return extension_bag_not_ready_error_locked();
    if (g_module_view_installed) return op_err("extension view open; movement disabled (P2)");
    if (from_bag == 5 || to_bag == 5) return op_err("task bag excluded");
    if (from_slot < 0 || from_slot >= virtual_bag::kSlotCount) return op_err("bad slot");
    const bool from_original = from_bag >= 0 && from_bag < 6;
    const bool to_original = to_bag >= 0 && to_bag < 6;
    const int from_internal = extension_internal_bag(from_bag);
    const int to_internal = extension_internal_bag(to_bag);
    const bool from_extension = from_internal >= 0;
    const bool to_extension = to_internal >= 0;
    if (!from_original && !from_extension) return op_err("bad from bag");
    if (!to_original && !to_extension) return op_err("bad to bag");
    if (from_original && to_original) return op_err("use /api/item/inventory/move_item");
    std::lock_guard<std::mutex> lock(g_virtual_bag_mtx);
    ensure_state_loaded_locked();
    bool moved = false;
    if (from_original && to_extension) {
        moved = move_original_to_extension_slot_locked(from_bag, from_slot, to_internal);
    } else if (from_extension && to_original) {
        moved = move_extension_to_original_locked(from_internal, from_slot, to_bag);
    } else {
        moved = move_extension_to_extension_locked(from_internal, from_slot, to_internal, to_slot);
    }
    if (!moved) return op_err("move failed");
    return "{\"ok\":true,\"state\":" + extension_bag_status_json_locked() + "}";
}

std::string virtual_bag_inventory_bags_json() {
    if (!g_virtual_bag_enabled.load() || !game_in_world()) return "";
    std::lock_guard<std::mutex> lock(g_virtual_bag_mtx);
    ensure_state_loaded_locked();
    std::string out;
    for (int bag = 0; bag < virtual_bag::kBagCount; ++bag) {
        const int capacity = g_virtual_bag_state.capacities[bag];
        int filled = 0;
        std::string items;
        for (int slot = 0; slot < virtual_bag::kSlotCount; ++slot) {
            const virtual_bag::Item& item = g_virtual_bag_state.items[bag][slot];
            if (item.category <= 0 || item.count <= 0) continue;
            if (filled > 0) items += ",";
            items += "{\"slot\":" + std::to_string(slot) +
                     ",\"category\":" + std::to_string(item.category) +
                     ",\"count\":" + std::to_string(item.count) + "}";
            ++filled;
        }
        out += ",{\"bag\":" + std::to_string(bag + 6) +
               ",\"items\":[" + items + "]" +
               ",\"capacity\":" + std::to_string(capacity) +
               ",\"slot_count\":" + std::to_string(filled) + "}";
    }
    return out;
}
