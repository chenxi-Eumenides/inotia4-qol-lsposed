#include "game_ui_virtbag.h"

#include "contracts/game_host.h"
#include "contracts/host_sdk.h"
#include "contracts/ui.h"
#include "contracts/ownership.h"
#include "contracts/inventory_refresh.h"
#include "contracts/item_facts.h"
#include "plugins/extension_bag/extension_bag_equip_rules.h"
#include "plugins/extension_bag/extension_bag_port.h"
#include "plugins/extension_bag/move_merge.h"
#include "plugins/extension_bag/model/ownership_ledger.h"
#include "plugins/extension_bag/model/item_handle_custody.h"
#include "plugins/extension_bag/model/sale_transaction.h"
#include "plugins/extension_bag/model/draw_state_lock.h"
#include "plugins/extension_bag/model/main_menu_cleanup_edge.h"
#include "plugins/extension_bag/model/virtual_bag_state.h"
#include "contracts/module_save_port.h"
#include "contracts/popup_enter_alias.h"
#include "plugins/extension_bag/extension_bag_context.h"
#include "plugins/extension_bag/extension_bag_geometry.h"
#include "plugins/extension_bag/extension_bag_logging.h"
#include "plugins/extension_bag/plugin.h"
#include "plugins/extension_bag/store_sell_rules.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <functional>
#include <mutex>
#include <thread>
#include <unistd.h>

bool apply_extension_material_to_slot_locked(std::unique_lock<std::mutex>& lock,
                                             int src_bag, int src_slot, int dst_bag,
                                             int dst_slot, void* material, void* target_item);

// 共享锁与 dirty 标记在文件作用域（非匿名 namespace）：extension_bag_persistence.cpp
// 经 extern 引用同一实例（save.slot_data 回调/排空面与 UI 面共享锁边界，rulebook §2）。
std::mutex g_virtual_bag_mtx;
bool g_item_state_dirty = false;
std::atomic<uint64_t> g_item_state_revision{0};

void extension_bag_mark_dirty_locked() {
    g_item_state_dirty = true;
    g_item_state_revision.fetch_add(1, std::memory_order_release);
}

uint64_t extension_bag_item_revision_locked() {
    return g_item_state_revision.load(std::memory_order_acquire);
}

namespace {

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
constexpr int kEquipSlotCount = 10;
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

// Extension source protection follows the extension_bag gate independently of move_merge.
std::atomic<bool> g_extension_source_protection_requested{false};
// 生命周期节拍帧任务（D2a-3c：原 150ms 线程体迁 infra::scheduler，主线程执行）。
uint64_t g_lifecycle_task = 0;
const qol::contracts::QolHostSdk* g_sdk = nullptr;
const qol::contracts::QolGameHost* g_game = nullptr;
const qol::contracts::QolExtensionBagWritePort* g_write_port = nullptr;
const qol::contracts::QolCharacterExtrasPort* g_character_extras = nullptr;
const qol::contracts::QolInventoryExtrasPort* g_inventory_extras = nullptr;
const qol::contracts::ItemFactsApi* g_item_facts = nullptr;
const qol::contracts::CraftFactsApi* g_craft_facts = nullptr;
int g_lifecycle_last_state = -1;
std::atomic<bool> g_main_menu_cleanup_dirty{false};
std::atomic<bool> g_virtual_bag_enabled{false};
std::atomic<uint64_t> g_exit_trace_sequence{0};
jclass g_virtual_bag_bridge_class = nullptr;
uint8_t* g_state_entry = nullptr;
PopupEventFn g_orig_event = nullptr;
PopupNoArgFn g_orig_f3 = nullptr;
PopupNoArgFn g_orig_enter = nullptr;
uintptr_t g_inventory_enter = 0;
PopupNoArgFn g_orig_save_enter = nullptr;
uint8_t* g_save_state_entry = nullptr;
bool g_save_panel_hook_installed = false;
void* g_backup_save_inventory = nullptr;
virtual_bag::State g_virtual_bag_state{};
int g_loaded_slot = -2;

uint64_t isolation_now_ms_locked();
std::array<std::array<void*, virtual_bag::kSlotCount>, virtual_bag::kBagCount> g_module_objects{};
struct ModuleUseState {
    bool active = false;
    bool consumed = false;
    bool pending_release = false;
    void* item = nullptr;
    uint64_t generation = 0;
    std::thread::id owner{};
};
struct ModuleUseToken {
    int bag = -1;
    int slot = -1;
    void* item = nullptr;
    uint64_t generation = 0;
    std::thread::id owner{};
};
struct ModuleUseReleasePlan {
    bool pending = false;
    int bag = -1;
    int slot = -1;
    void* item = nullptr;
    void* control = nullptr;
    uint32_t handle = 0;
    uint64_t generation = 0;
};
std::array<std::array<ModuleUseState, virtual_bag::kSlotCount>, virtual_bag::kBagCount>
    g_module_object_use{};
thread_local ModuleUseToken* g_active_module_use_token = nullptr;
std::array<std::array<int, virtual_bag::kSlotCount>, virtual_bag::kBagCount> g_module_object_categories{};
std::array<std::array<uint32_t, virtual_bag::kSlotCount>, virtual_bag::kBagCount> g_module_object_hashes{};
extern uint32_t g_module_object_handles[virtual_bag::kBagCount][virtual_bag::kSlotCount];
std::array<void*, virtual_bag::kSlotCount> g_original_inventory{};
uint8_t g_original_current_direct = 0;
uint8_t g_original_current_got = 0;
int g_original_bag_size_bag = -1;
uint32_t g_original_bag_size = 0;
int g_module_window_original_bag = -1;
void* g_projected_item_root = nullptr;
bool g_module_view_installed = false;
int g_module_view_index = -1;
void* g_pending_jewel_detail_item = nullptr;
int g_pending_jewel_detail_bag = -1;
int g_pending_jewel_detail_slot = -1;
uint8_t g_exit_display_bag = kNoOriginalBagSelected;
bool g_inventory_frame_active = false;
bool g_extension_touch_capture = false;
std::array<void*, virtual_bag::kBagCount> g_extension_tab_buttons{};
void* g_extension_tab_root = nullptr;
uint64_t g_inventory_generation = 0;
uint64_t g_extension_tab_generation = 0;
int g_pending_extension_tab = -1;
uint64_t g_pending_extension_tab_generation = 0;
int g_extension_desc_unequip_bag = -1;
std::array<void*, 11> g_extension_desc_item_originals{};
std::array<bool, 11> g_extension_desc_item_bound{};
void* g_extension_desc_item = nullptr;
int g_extension_desc_item_bag = -1;
int g_extension_desc_item_slot = -1;
using ExtensionDestroyOkFn = void (*)(void*);
ExtensionDestroyOkFn g_extension_destroy_original_ok = nullptr;
ExtensionDestroyOkFn g_extension_sell_original_ok = nullptr;
ExtensionDestroyOkFn g_extension_destroy_original_cancel = nullptr;
ExtensionDestroyOkFn g_extension_sell_original_cancel = nullptr;
bool g_extension_sell_apply_variant_discount = true;
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
virtual_bag::ExtensionDragSession g_extension_drag_session{};

std::atomic<uint64_t> g_p5_observation_sequence{0};
std::atomic<uint64_t> g_p5_last_item_query_sample_ms{0};
std::atomic<uint64_t> g_p5_last_panel_move_sample_ms{0};
thread_local bool g_p5_observation_active = false;
thread_local bool g_refresh_restore_suppress = false;
thread_local bool g_in_native_call = false;
void* g_tab_bag_items[virtual_bag::kBagCount] = {};

bool read_item_category(const void* item, int* out_category) {
    qol::contracts::QolItemHandle handle{};
    if (g_item_facts == nullptr || g_item_facts->category == nullptr ||
        item == nullptr || out_category == nullptr ||
        !extension_bag_item_handle_for_pointer(const_cast<void*>(item), &handle)) return false;
    int32_t category = 0;
    if (!qol::contracts::succeeded(
            g_item_facts->category(handle, &category))) return false;
    *out_category = category;
    return true;
}

const qol::contracts::QolExtensionBagWritePort* extension_bag_write_api() { return g_write_port; }

bool extension_save_item_on_empty(void* item, int bag) {
    const auto* api = extension_bag_write_api();
    if (api == nullptr || api->inventory.save_item_on_empty == nullptr) return false;
    int32_t result = 0;
    return qol::contracts::succeeded(api->inventory.save_item_on_empty(item, bag, &result)) &&
           result != 0;
}

bool extension_equip_from_inventory(void* character, int bag, int slot, int equip_slot) {
    const auto* api = extension_bag_write_api();
    if (api == nullptr || api->equipment.equip_from_inventory == nullptr) return false;
    int32_t result = 0;
    return qol::contracts::succeeded(
               api->equipment.equip_from_inventory(character, bag, slot, equip_slot, &result)) &&
           result != 0;
}

bool extension_apply_stuff(void* target_item, void* material) {
    const auto* api = extension_bag_write_api();
    return api != nullptr && api->equipment.apply_stuff != nullptr &&
           qol::contracts::succeeded(api->equipment.apply_stuff(target_item, material));
}

bool extension_use_item(void* character, void* item, int arg, int* out_used) {
    const auto* api = extension_bag_write_api();
    if (api == nullptr || api->equipment.use_item == nullptr || out_used == nullptr) return false;
    int32_t result = 0;
    const QolResult call = api->equipment.use_item(character, item, arg, &result);
    if (!qol::contracts::succeeded(call)) return false;
    *out_used = result;
    return true;
}

bool extension_make_mercenary(void* item, void** out_mercenary) {
    const auto* api = extension_bag_write_api();
    return api != nullptr && api->equipment.make_mercenary != nullptr &&
           qol::contracts::succeeded(api->equipment.make_mercenary(item, out_mercenary));
}

bool extension_add_money(int64_t amount) {
    const auto* api = extension_bag_write_api();
    if (api == nullptr || api->equipment.add_money == nullptr) return false;
    int32_t result = 0;
    return qol::contracts::succeeded(api->equipment.add_money(amount, &result)) && result != 0;
}

bool extension_minus_money(int64_t amount) {
    const auto* api = extension_bag_write_api();
    if (api == nullptr || api->equipment.minus_money == nullptr) return false;
    int32_t result = 0;
    return qol::contracts::succeeded(api->equipment.minus_money(amount, &result)) && result != 0;
}

bool extension_move_item(void* item, int count, int bag, int slot, int* out_result) {
    const auto* api = extension_bag_write_api();
    if (api == nullptr || api->inventory.move_item == nullptr || out_result == nullptr) return false;
    int32_t result = 0;
    const QolResult call = api->inventory.move_item(item, count, bag, slot, &result);
    if (!qol::contracts::succeeded(call)) return false;
    *out_result = result;
    return true;
}

bool extension_remove_item_direct(int bag, int slot, int* out_result) {
    const auto* api = extension_bag_write_api();
    if (api == nullptr || api->inventory.remove_item_direct == nullptr || out_result == nullptr) {
        return false;
    }
    int32_t result = 0;
    const QolResult call = api->inventory.remove_item_direct(bag, slot, &result);
    if (!qol::contracts::succeeded(call)) return false;
    *out_result = result;
    return true;
}

bool extension_free_item(void* item) {
    const auto* api = extension_bag_write_api();
    return api != nullptr && api->inventory.free_item != nullptr &&
           qol::contracts::succeeded(api->inventory.free_item(item));
}

void* extension_create_item(int category, int arg1, int arg2, int arg3) {
    const auto* api = extension_bag_write_api();
    if (api == nullptr || api->inventory.create_item == nullptr) return nullptr;
    void* item = nullptr;
    return qol::contracts::succeeded(
               api->inventory.create_item(category, arg1, arg2, arg3, &item))
               ? item
               : nullptr;
}

bool extension_serialize_item(uint8_t* payload, int payload_capacity, void* item, int* out_size) {
    const auto* api = extension_bag_write_api();
    return api != nullptr && api->inventory.serialize_item != nullptr && out_size != nullptr &&
           qol::contracts::succeeded(
               api->inventory.serialize_item(payload, payload_capacity, item, out_size));
}

void* extension_load_item(const uint8_t* payload, int payload_size, int* out_consumed) {
    const auto* api = extension_bag_write_api();
    if (api == nullptr || api->inventory.load_item == nullptr || out_consumed == nullptr) {
        return nullptr;
    }
    void* item = nullptr;
    const QolResult result =
        api->inventory.load_item(payload, payload_size, &item, out_consumed);
    if (qol::contracts::succeeded(result)) return item;
    if (item != nullptr) (void)extension_free_item(item);
    return nullptr;
}

int extension_serialize_item_payload(uint8_t* payload, void* item) {
    int size = 0;
    return extension_serialize_item(payload, virtual_bag::kSerializedItemProbeBuffer, item, &size)
               ? size
               : 0;
}

int extension_load_item_payload(const uint8_t* payload, void** out_item, int* out_consumed) {
    if (out_item == nullptr || out_consumed == nullptr) return 0;
    *out_item = extension_load_item(payload, virtual_bag::kSerializedItemBuffer, out_consumed);
    return *out_item == nullptr ? 0 : 1;
}

void extension_free_item_payload(void* item) { (void)extension_free_item(item); }

void extension_refresh_item_area() {
    const auto* api = extension_bag_write_api();
    if (api != nullptr && api->ui.refresh_item_area != nullptr) {
        (void)api->ui.refresh_item_area();
    }
}

void extension_refresh_bag_area() {
    const auto* api = extension_bag_write_api();
    if (api != nullptr && api->ui.refresh_bag_area != nullptr) {
        (void)api->ui.refresh_bag_area();
    }
}

void extension_refresh_mix_item() {
    const auto* api = extension_bag_write_api();
    if (api != nullptr && api->ui.refresh_mix_item != nullptr) {
        (void)api->ui.refresh_mix_item();
    }
}

void extension_refresh_store_item() {
    const auto* api = extension_bag_write_api();
    if (api != nullptr && api->ui.refresh_store_item != nullptr) {
        (void)api->ui.refresh_store_item();
    }
}

void extension_update_character_equip() {
    const auto* api = extension_bag_write_api();
    if (api != nullptr && api->ui.update_character_equip != nullptr) {
        (void)api->ui.update_character_equip();
    }
}

void extension_description_set_off() {
    const auto* api = extension_bag_write_api();
    if (api != nullptr && api->ui.description_set_off != nullptr) {
        (void)api->ui.description_set_off();
    }
}

bool read_item_cumulate_count(const void* item, int* out_count) {
    qol::contracts::QolItemHandle handle{};
    if (g_item_facts == nullptr || g_item_facts->cumulate_count == nullptr ||
        item == nullptr || out_count == nullptr ||
        !extension_bag_item_handle_for_pointer(const_cast<void*>(item), &handle)) return false;
    int32_t count = 0;
    if (!qol::contracts::succeeded(
            g_item_facts->cumulate_count(handle, &count))) return false;
    *out_count = count;
    return true;
}

bool read_item_is_jewel(int category) {
    if (g_item_facts == nullptr || g_item_facts->is_jewel_category == nullptr) return false;
    int32_t is_jewel = 0;
    return qol::contracts::succeeded(
               g_item_facts->is_jewel_category(category, &is_jewel)) &&
           is_jewel != 0;
}

bool read_item_category_predicate(int category,
                                  QolResult (*predicate)(int32_t, int32_t*),
                                  bool* out_value) {
    if (g_game == nullptr || predicate == nullptr || out_value == nullptr) return false;
    int32_t value = 0;
    if (!qol::contracts::succeeded(predicate(category, &value))) return false;
    *out_value = value != 0;
    return true;
}

bool item_is_equip(const void* item) {
    qol::contracts::QolItemHandle handle{};
    if (g_item_facts == nullptr || g_item_facts->is_real_equip == nullptr || item == nullptr ||
        !extension_bag_item_handle_for_pointer(const_cast<void*>(item), &handle)) {
        return false;
    }
    int32_t value = 0;
    return qol::contracts::succeeded(g_item_facts->is_real_equip(handle, &value)) &&
           value != 0;
}

bool read_item_is_enchant_scroll(int category) {
    bool value = false;
    return read_item_category_predicate(
        category, g_game == nullptr ? nullptr : g_game->read_only.item.is_enchant_scroll, &value) &&
        value;
}

bool read_item_is_mercenary_seal(int category) {
    bool value = false;
    return read_item_category_predicate(
        category, g_game == nullptr ? nullptr : g_game->read_only.item.is_mercenary_seal, &value) &&
        value;
}

bool read_item_is_usable(int category) {
    bool value = false;
    return read_item_category_predicate(
        category, g_game == nullptr ? nullptr : g_game->read_only.item.is_usable, &value) &&
        value;
}

bool read_world_state_id(int* out_state) {
    if (out_state == nullptr) return false;
    const auto* facts = qol::features::extension_bag::world_facts();
    if (facts == nullptr || facts->state == nullptr) return false;
    int32_t state = -1;
    if (!qol::contracts::succeeded(facts->state(&state))) return false;
    *out_state = state;
    return true;
}

bool read_world_in_world() {
    int state = -1;
    return read_world_state_id(&state) && state == 5;
}

const qol::contracts::QolItemCountApi* item_count_api();

bool mark_native_bag_object(void* item) {
    const auto* api = item_count_api();
    if (api == nullptr || api->read_encoded_field == nullptr ||
        api->write_encoded_field == nullptr || item == nullptr) return false;
    uint32_t field = 0;
    if (!qol::contracts::succeeded(api->read_encoded_field(item, &field))) return false;
    field = qol::features::extension_bag::write_native_bag_object_marker(field);
    return qol::contracts::succeeded(api->write_encoded_field(item, field));
}

bool read_item_sell_price(const void* item, int64_t* out_price) {
    qol::contracts::QolItemHandle handle{};
    if (g_item_facts == nullptr || g_item_facts->sell_price == nullptr ||
        item == nullptr || out_price == nullptr ||
        !extension_bag_item_handle_for_pointer(const_cast<void*>(item), &handle)) return false;
    return qol::contracts::succeeded(g_item_facts->sell_price(handle, out_price));
}

bool read_item_encoded_field(const void* item, uint32_t* out_field) {
    const auto* api = item_count_api();
    if (api == nullptr || api->read_encoded_field == nullptr || item == nullptr ||
        out_field == nullptr) return false;
    return qol::contracts::succeeded(api->read_encoded_field(item, out_field));
}

qol::contracts::QolItemCountEncoding item_count_encoding(const void* item) {
    const auto* api = item_count_api();
    if (api == nullptr || api->is_encoded == nullptr || item == nullptr) {
        return qol::contracts::QolItemCountEncoding::kUnknown;
    }
    qol::contracts::QolItemCountEncoding encoding =
        qol::contracts::QolItemCountEncoding::kUnknown;
    if (!qol::contracts::succeeded(api->is_encoded(item, &encoding))) {
        return qol::contracts::QolItemCountEncoding::kUnknown;
    }
    return encoding;
}

bool read_char_can_equip(const void* character, const void* item) {
    if (g_game == nullptr || g_character_extras == nullptr || g_character_extras->read.can_equip == nullptr) return false;
    int32_t allowed = 0;
    return qol::contracts::succeeded(
               g_character_extras->read.can_equip(character, item, &allowed)) &&
           allowed != 0;
}

bool read_char_find_equip_slot(const void* character, const void* item, int* out_slot) {
    if (g_game == nullptr || g_character_extras == nullptr || g_character_extras->read.find_equip_slot == nullptr ||
        out_slot == nullptr) return false;
    int32_t slot = -1;
    if (!qol::contracts::succeeded(
            g_character_extras->read.find_equip_slot(character, item, &slot))) return false;
    *out_slot = slot;
    return true;
}

void* read_char_equipped_item(const void* character, int slot) {
    if (g_game == nullptr || g_character_extras == nullptr || g_character_extras->read.equipped_item == nullptr) return nullptr;
    void* item = nullptr;
    return qol::contracts::succeeded(
               g_character_extras->read.equipped_item(character, slot, &item))
               ? item
               : nullptr;
}

bool read_save_is_ok() {
    const auto* facts = qol::features::extension_bag::save_facts();
    if (facts == nullptr || facts->is_ok == nullptr) return false;
    int32_t value = 0;
    return qol::contracts::succeeded(facts->is_ok(&value)) && value != 0;
}

bool read_has_empty_mercenary_slot() {
    if (g_game == nullptr || g_character_extras == nullptr || g_character_extras->read.has_empty_mercenary_slot == nullptr) {
        return false;
    }
    int32_t value = 0;
    return qol::contracts::succeeded(
               g_character_extras->read.has_empty_mercenary_slot(&value)) && value != 0;
}

bool read_inventory_bag_size(int bag, int* out_size) {
    if (g_game == nullptr || g_inventory_extras == nullptr || g_inventory_extras->read.bag_size == nullptr ||
        out_size == nullptr) return false;
    int32_t value = 0;
    if (!qol::contracts::succeeded(g_inventory_extras->read.bag_size(bag, &value))) return false;
    *out_size = value;
    return true;
}

bool read_inventory_is_empty_bag(int bag) {
    if (g_game == nullptr || g_inventory_extras == nullptr || g_inventory_extras->read.is_empty_bag == nullptr) return false;
    int32_t value = 0;
    return qol::contracts::succeeded(
               g_inventory_extras->read.is_empty_bag(bag, &value)) && value != 0;
}

bool read_inventory_find_save_slot(const void* item, int* out_slot) {
    if (g_game == nullptr || g_inventory_extras == nullptr || g_inventory_extras->read.find_save_slot == nullptr ||
        item == nullptr || out_slot == nullptr) return false;
    int32_t value = -1;
    if (!qol::contracts::succeeded(
            g_inventory_extras->read.find_save_slot(item, &value))) return false;
    *out_slot = value;
    return true;
}

int read_current_save_slot() {
    const auto* facts = qol::features::extension_bag::save_facts();
    if (facts == nullptr || facts->current_slot == nullptr) return -1;
    int32_t slot = -1;
    return qol::contracts::succeeded(facts->current_slot(&slot)) ? slot : -1;
}

struct NativeCallScope {
    const bool previous = g_in_native_call;
    const qol::contracts::QolGameHost* game = g_game;

    NativeCallScope() {
        g_in_native_call = true;
        if (game != nullptr && game->item_abi.begin_original_only != nullptr) {
            game->item_abi.begin_original_only();
        }
    }
    ~NativeCallScope() {
        if (game != nullptr && game->item_abi.end_original_only != nullptr) {
            game->item_abi.end_original_only();
        }
        g_in_native_call = previous;
    }
};

int extension_tab_index(void* ctrl);
bool module_use_begin_locked(int bag, int slot, void* item, ModuleUseToken* out_token);
bool module_use_finish_locked(const ModuleUseToken& token, bool* out_consumed,
                              ModuleUseReleasePlan* out_release);
bool module_use_abort_locked(const ModuleUseToken& token);
bool module_use_abort_isolate_locked(const ModuleUseToken& token);
bool module_use_release_native(const ModuleUseReleasePlan& release);
bool module_use_finalize_release_locked(const ModuleUseReleasePlan& release);
bool module_slot_is_assignable_locked(int bag, int slot);
bool module_object_replacement_allowed_locked(int bag, int slot, const char* context);
bool module_object_generation_advance_locked(int bag, int slot, const char* context);
bool module_object_release_allowed_locked(int bag, int slot, void* item, const char* context,
                                          bool allow_owner, const ModuleUseToken* token);
void store_teardown_locked();
void mix_teardown_locked();

#include "plugins/extension_bag/extension_bag_observation.inc"

void extension_tab_button_clicked(void* ctrl);

#include "plugins/extension_bag/extension_bag_tabs.inc"

// 扩展标签控件事件处理：TouchHandle 转发的控件事件（0x02=click 松开确认，
// 0x04=drop 到袋）。挂袋容器后与原版袋标签同链：松开 → TouchHandle 判定。
void queue_extension_tab_click_locked(int extension_bag);
void* touch_moving_item_control_locked();
bool try_equip_on_extension_tab_drop_locked(std::unique_lock<std::mutex>& lock,
                                            int index, void* moving_control);
bool extension_source_should_equip_locked(int target_bag);
bool equip_extension_source_on_tab_locked(std::unique_lock<std::mutex>& lock, int target_bag,
                                          int source_bag, int source_slot);
bool move_extension_to_extension_locked(int src_bag, int src_slot, int dst_bag, int dst_slot);
bool persist_state_locked(bool force);
void refresh_projected_module_view_after_move_locked();
void cancel_projected_drag_session_locked(const char* reason);
void refresh_projection_if_overwritten_locked();
bool sync_projected_slot_locked(int bag, int slot);
bool route_projected_session_to_tab_locked(std::unique_lock<std::mutex>& lock, int target_bag);
int original_item_category_locked(void* item);
int drag_source_category_locked();
bool move_original_to_extension_locked(std::unique_lock<std::mutex>& lock, int dst_bag,
                                       void* moving_control);
void* load_item_payload_tracked_locked(const uint8_t* payload, int payload_size,
                                       const char* context, uint32_t* out_handle);
void handover_tracked_item_locked(uint32_t handle, void* item, const char* context);
void release_tracked_item_locked(uint32_t handle, void* item, const char* context);
bool inventory_slot_locked(int bag, int slot, void** out);

bool remove_item_direct_unlocked(std::unique_lock<std::mutex>& lock, int bag, int slot,
                                 void* expected_item, const char* context) {
    if (!lock.owns_lock() || extension_bag_write_api() == nullptr ||
        extension_bag_write_api()->inventory.remove_item_direct == nullptr ||
        expected_item == nullptr || g_item_facts == nullptr ||
        g_item_facts->item_ref_at == nullptr) {
        return false;
    }

    void* source_item = nullptr;
    qol::contracts::QolInventoryItemRef source_ref{};
    qol::contracts::QolItemHandle source_handle{};
    if (!inventory_slot_locked(bag, slot, &source_item) || source_item != expected_item ||
        !qol::contracts::succeeded(g_item_facts->item_ref_at(bag, slot, &source_ref)) ||
        source_ref.bag != bag || source_ref.slot != slot ||
        source_ref.item.slot == 0 || source_ref.item.generation == 0 ||
        !extension_bag_item_handle_for_pointer(expected_item, &source_handle) ||
        source_handle.slot != source_ref.item.slot ||
        source_handle.generation != source_ref.item.generation) {
        VIRTBAG_LOG_ERROR(
            "ERROR native remove source identity unavailable context=%s bag=%d slot=%d expected=%p actual=%p ref_bag=%d ref_slot=%d handle=%u/%u",
            context != nullptr ? context : "unknown", bag, slot, expected_item, source_item,
            source_ref.bag, source_ref.slot, source_ref.item.slot,
            source_ref.item.generation);
        return false;
    }

    bool remove_call_ok = false;
    int result = 0;
    lock.unlock();
    {
        NativeCallScope native_call;
        remove_call_ok = extension_remove_item_direct(bag, slot, &result);
    }
    lock.lock();

    void* actual_item = nullptr;
    const bool readable = inventory_slot_locked(bag, slot, &actual_item);
    qol::contracts::QolInventoryItemRef actual_ref{};
    const bool identity_readable = qol::contracts::succeeded(
        g_item_facts->item_ref_at(bag, slot, &actual_ref));
    const bool identity_at_source = identity_readable &&
        actual_ref.bag == bag && actual_ref.slot == slot;
    const bool source_handle_cleared = identity_at_source &&
        actual_ref.item.slot == 0 && actual_ref.item.generation == 0;
    const bool removed = readable && identity_at_source && source_handle_cleared &&
                         virtual_bag::original_to_extension_source_slot_postcondition(
                             expected_item, actual_item);
    if (!remove_call_ok) {
        VIRTBAG_LOG_ERROR(
            "ERROR native remove call failed context=%s bag=%d slot=%d result=%d; checking source identity postcondition",
            context != nullptr ? context : "unknown", bag, slot, result);
    }
    if (!removed) {
        VIRTBAG_LOG_ERROR(
                    "ERROR native remove postcondition failed context=%s bag=%d slot=%d expected=%p actual=%p readable=%d source_handle=%u/%u identity_readable=%d ref_bag=%d ref_slot=%d actual_handle=%u/%u",
                    context != nullptr ? context : "unknown", bag, slot, expected_item,
                    actual_item, readable ? 1 : 0, source_handle.slot,
                    source_handle.generation, identity_readable ? 1 : 0,
                    actual_ref.bag, actual_ref.slot, actual_ref.item.slot,
                    actual_ref.item.generation);
    }
    return removed;
}
bool consume_extension_item_after_native_locked(int bag, int slot, void* item,
                                                int before_count, int observed_before,
                                                const char* context, void* use_token);

#include "plugins/extension_bag/extension_bag_drag_session.inc"

#include "plugins/extension_bag/extension_bag_runtime.inc"

const qol::contracts::QolItemCountApi* item_count_api() {
    return g_game == nullptr ? nullptr : &g_game->item_count;
}

uint32_t original_count_view(uint32_t field) {
    return virtual_bag_view_from_canonical(virtual_bag_canonical_from_field(field, 0), false);
}

}  // namespace

namespace qol::features::extension_bag {

bool extension_bag_bind_host(const qol::contracts::QolHostSdk* sdk,
                             const qol::contracts::QolGameHost* game,
                             const qol::contracts::QolExtensionBagWritePort* write_port) {
    if (sdk == nullptr || game == nullptr || sdk->ui == nullptr ||
        write_port == nullptr ||
        write_port->abi_version.major != qol::contracts::kQolPortExtensionBagWriteVersion.major ||
        write_port->abi_version.minor != qol::contracts::kQolPortExtensionBagWriteVersion.minor ||
        sdk->abi.size < sizeof(qol::contracts::QolHostSdk) ||
        sdk->abi.major != qol::contracts::kHostSdkMajor ||
        game->abi.size < sizeof(qol::contracts::QolGameHost) ||
        game->abi.major != qol::contracts::kGameHostMajor ||
        sdk->ui->size < sizeof(qol::contracts::QolUiAdapter) ||
        sdk->ui->major != qol::contracts::kUiAdapterMajor ||
        sdk->hooks.popup_resolve == nullptr || sdk->hooks.popup_acquire == nullptr ||
        sdk->hooks.popup_restore == nullptr || sdk->desc_button.bind == nullptr ||
        sdk->desc_button.unbind == nullptr || sdk->desc_button.current_proc == nullptr ||
        sdk->desc_button.original_matches == nullptr || sdk->desc_button.call_original == nullptr ||
        sdk->scheduler.add == nullptr ||
        sdk->scheduler.remove == nullptr || sdk->capabilities.require == nullptr) {
        g_sdk = nullptr;
        g_game = nullptr;
        g_write_port = nullptr;
        g_character_extras = nullptr;
        g_inventory_extras = nullptr;
        return false;
    }
    g_sdk = sdk;
    g_game = game;
    g_write_port = write_port;
    return true;
}

void extension_bag_bind_character_extras(
    const qol::contracts::QolCharacterExtrasPort* port) {
    g_character_extras = port;
}

void extension_bag_bind_inventory_extras(
    const qol::contracts::QolInventoryExtrasPort* port) {
    g_inventory_extras = port;
}

const qol::contracts::QolGameHost* extension_bag_game_host() { return g_game; }
const qol::contracts::QolHostSdk* extension_bag_host_sdk() { return g_sdk; }

void extension_bag_bind_item_facts(const qol::contracts::ItemFactsApi* facts) {
    g_item_facts = facts;
}

const qol::contracts::ItemFactsApi* extension_bag_item_facts() { return g_item_facts; }

void extension_bag_bind_craft_facts(const qol::contracts::CraftFactsApi* facts) {
    g_craft_facts = facts;
}

const qol::contracts::CraftFactsApi* extension_bag_craft_facts() { return g_craft_facts; }

}  // namespace qol::features::extension_bag

uint32_t virtual_bag_effective_count(const void* item, uint32_t fallback_canonical) {
    const qol::contracts::QolItemCountApi* api = item_count_api();
    if (item != nullptr) {
        if (api != nullptr && api->effective != nullptr) {
            uint32_t count = 0;
            const QolResult result = api->effective(item, &count);
            if (qol::contracts::succeeded(result)) {
                // 关闭态 effective 视图可能为 0（canonical&0x7F==0），而 descriptor canonical
                // 已证明该槽有物；回 0 会被下游当非法 count 丢弃。此时取 canonical 视图
                // （R-47 决策 b：启用态 canonical、关闭态 b）。真值：canonical=0→0；
                // effective>0→effective；effective==0 且 canonical>0→canonical 视图。
                if (count != 0 || fallback_canonical == 0) return count;
                return virtual_bag_view_from_canonical(fallback_canonical, /*extended=*/true);
            }
            // 任何失败（含 unsupported，如 ITEMCLASS 表不可解）都不回 0：回 0 会被下游
            // 当作非法 count 而使整段 EB 快照失效。改为回退原版视图（G-item-#10 口径：
            // 能力缺失或不可用时回退原版 getter）。
        }
        uint32_t field = 0;
        if (api != nullptr && api->read_encoded_field != nullptr &&
            qol::contracts::succeeded(api->read_encoded_field(item, &field))) {
            const uint32_t view = original_count_view(field);
            // 活体对象缺失/不可解时 field 可读但 b 段为 0（如 canonical 的低 7 位为 0）。
            // descriptor canonical 已证明该槽有物，回 0 会被下游当非法 count 丢弃。
            if (view != 0) return view;
        }
    }
    // 启用态输出 canonical 视图（R-47 决策 b：启用态 canonical、关闭态 b）；本函数仅在
    // EB 启用时经 for_each_logical_item 调用，故兜底取 canonical 视图而非 b 视图，
    // 避免 canonical&0x7F==0 的投影槽被当作非法 count 跳过。
    return virtual_bag_view_from_canonical(fallback_canonical, /*extended=*/true);
}

uint32_t virtual_bag_canonical_from_field(uint32_t field, uint32_t fallback_canonical) {
    const qol::contracts::QolItemCountApi* api = item_count_api();
    if (api != nullptr && api->canonical_from_field != nullptr) {
        uint32_t canonical = fallback_canonical;
        const QolResult result = api->canonical_from_field(field, &canonical);
        if (qol::contracts::succeeded(result)) return canonical;
    }
    return fallback_canonical;
}

uint32_t virtual_bag_field_from_canonical(uint32_t field, uint32_t canonical, bool extended) {
    const qol::contracts::QolItemCountApi* api = item_count_api();
    if (api != nullptr && api->field_from_canonical != nullptr) {
        uint32_t updated = field;
        const QolResult result = api->field_from_canonical(
            field, canonical, extended ? 1 : 0, &updated);
        if (qol::contracts::succeeded(result)) return updated;
    }
    return field;
}

uint32_t virtual_bag_view_from_canonical(uint32_t canonical, bool extended) {
    const qol::contracts::QolItemCountApi* api = item_count_api();
    if (api != nullptr && api->view_from_canonical != nullptr) {
        uint32_t view = canonical;
        const QolResult result = api->view_from_canonical(canonical, extended ? 1 : 0, &view);
        if (qol::contracts::succeeded(result)) return view;
    }
    return canonical;
}

uint32_t virtual_bag_count_limit(bool extended) {
    const qol::contracts::QolItemCountApi* api = item_count_api();
    if (api != nullptr && api->view_limit != nullptr) {
        uint32_t limit = 0;
        const QolResult result = api->view_limit(extended ? 1 : 0, &limit);
        if (qol::contracts::succeeded(result)) return limit;
    }
    return 0;
}

uint32_t virtual_bag_canonical_count(const void* item, uint32_t fallback_canonical) {
    const qol::contracts::QolItemCountApi* api = item_count_api();
    if (api != nullptr && api->canonical != nullptr && item != nullptr) {
        uint32_t count = 0;
        const QolResult result = api->canonical(item, &count);
        if (qol::contracts::succeeded(result)) return count;
        if (result.code == kQolErrUnsupported) {
            return fallback_canonical;
        }
    }
    if (item != nullptr) {
        uint32_t field = 0;
        if (api == nullptr || api->read_encoded_field == nullptr ||
            !qol::contracts::succeeded(api->read_encoded_field(item, &field))) {
            return fallback_canonical;
        }
        return virtual_bag_canonical_from_field(field, fallback_canonical);
    }
    return fallback_canonical;
}

bool virtual_bag_extended_mode(const void* item) {
    const qol::contracts::QolItemCountApi* api = item_count_api();
    if (item == nullptr || api == nullptr || api->read_extended_mode == nullptr) return false;
    // OK1 表示 999 请求运行且整组资源已实际应用；NotReady（请求未落实）与 OK0（明确关闭）
    // 都不进入扩展视图，避免把未落地的模式当作关闭而回退原版语义。
    uint8_t extended = 0;
    const QolResult mode_result = api->read_extended_mode(&extended);
    if (!qol::contracts::succeeded(mode_result) || extended == 0) return false;
    if (api->effective == nullptr) return false;
    uint32_t effective = 0;
    const QolResult effective_result = api->effective(item, &effective);
    return qol::contracts::succeeded(effective_result);
}

bool virtual_bag_write_effective(void* item, uint32_t count) {
    const qol::contracts::QolItemCountApi* api = item_count_api();
    if (api == nullptr || api->write_effective == nullptr ||
        api->writeback_needed == nullptr || item == nullptr) {
        return false;
    }
    int32_t needed = 0;
    const QolResult needed_result = api->writeback_needed(item, count, &needed);
    // NotReady 表示 999 请求已开但资源组/适配尚未落地。原版写法对 canonical 的安全性未
    // 证明，故不回退 original_count_write；直接使该写请求失败并保留原错误语义。
    if (needed_result.code == kQolErrNotReady) return false;
    if (!qol::contracts::succeeded(needed_result)) return false;
    if (needed == 0) return true;
    const QolResult write_result = api->write_effective(item, count);
    if (write_result.code == kQolErrNotReady) return false;
    return qol::contracts::succeeded(write_result);
}

bool virtual_bag_native_call_active() {
    return g_in_native_call;
}

void virtual_bag_call_original_refresh_item_area_with_guard() {
    NativeCallScope native_call;
    // D2c-2b-1：经 inventory_refresh capability 现取 raw-original trampoline；能力缺失
    // （provider 未安装/撤销中）按既有 no-op 降级——不直呼 fn_ui_equip_refresh_item_area，
    // 否则会经已被 hook 的入口回入 wrapper（原 trampoline 存在的唯一原因）。
    const void* refresh_iface = nullptr;
    if (g_sdk == nullptr || g_sdk->capabilities.require == nullptr) return;
    const QolResult refresh_result = g_sdk->capabilities.require(
        qol::contracts::kInventoryRefreshCapabilityId,
        QolVersionRange{static_cast<uint16_t>(qol::contracts::kInventoryRefreshVersionMajor),
                        static_cast<uint16_t>(qol::contracts::kInventoryRefreshVersionMinor)},
        &refresh_iface);
    const auto* refresh = refresh_result.ok == kQolOk
        ? static_cast<const qol::contracts::InventoryRefreshApi*>(refresh_iface) : nullptr;
    if (refresh != nullptr && refresh->call_original != nullptr) {
        refresh->call_original();
    }
}

// 与 item_count_encoding/ITEM_GetCumulateCount 使用同一 ITEMCLASSBASE +6 bit0 语义：
// kUnknown 必须继续向载入/合并调用方传播，不能降级成可堆叠。
::qol::contracts::QolItemCountEncoding virtual_bag_category_uses_stack_count(int category) {
    return category_is_equip(category);
}

void virtual_bag_refresh_item_area_with_gate() {
    std::lock_guard<std::mutex> lock(g_virtual_bag_mtx);
    virtual_bag_call_original_refresh_item_area_with_guard();
    const bool projection_installed =
        g_module_view_installed && g_projected_item_root != nullptr &&
        virtual_bag::valid_index(g_module_view_index);
    if (!g_refresh_restore_suppress && projection_installed) {
        refresh_projection_if_overwritten_locked();
    }
}

VirtualBagEquipControlEventResult virtual_bag_handle_equip_control_event(
    void* control, uint64_t event, void* x2, void* param,
    VirtualBagEquipControlEventBackup backup, uint64_t* out_result) {
    if (out_result != nullptr) *out_result = 0;
    if (event != 0x04 || param == nullptr) {
        return VirtualBagEquipControlEventResult::kNotExtension;
    }
    void* source_control = *reinterpret_cast<void**>(reinterpret_cast<uint8_t*>(param) + 8);
    void* source_item = nullptr;
    void* target_item = nullptr;
    int source_bag = -1;
    int source_slot = -1;
    int target_slot_index = -1;
    ModuleUseToken use_token{};
    {
        std::lock_guard<std::mutex> lock(g_virtual_bag_mtx);
        ensure_state_loaded_locked();
        // 控件指针只作为当前投影索引；data 和 item 必须在同一锁上下文重读，
        // 防止控件在锁外被重绑后仍沿用旧身份。
        void* source_data = source_control != nullptr
            ? extension_bag_ui_data(source_control) : nullptr;
        void* target_data = control != nullptr ? extension_bag_ui_data(control) : nullptr;
        source_item = source_data != nullptr ? *reinterpret_cast<void**>(source_data) : nullptr;
        target_item = target_data != nullptr ? *reinterpret_cast<void**>(target_data) : nullptr;
        target_slot_index = control != nullptr ? extension_bag_ui_item_slot(control) : -1;
        const bool extension_source =
            source_item != nullptr && module_slot_of_item_locked(source_item, &source_bag, &source_slot);
        const int source_category = extension_source
            ? g_virtual_bag_state.items[source_bag][source_slot].category : 0;
        const bool source_is_apply_material = extension_source &&
            (read_item_is_jewel(source_category) ||
             read_item_is_enchant_scroll(source_category));
        // H-15 装备槽事件适配（R-50）：目标必须是装备类（item_is_equip 判定；
        // 类别数据不可用时 item_is_equip 为假，按非装备 fail-closed 直通原版
        // backup，不得 Blocked），且落点是装备槽控件（slot index >= 16）。
        const bool target_is_equip_slot = target_item != nullptr &&
            item_is_equip(target_item) && target_slot_index >= 16;
        if (!extension_bag_is_equip_control_source(
                event, extension_source, source_is_apply_material, target_is_equip_slot)) {
            return VirtualBagEquipControlEventResult::kNotExtension;
        }

        const virtual_bag::Item& descriptor = g_virtual_bag_state.items[source_bag][source_slot];
        const ModuleUseState& use = g_module_object_use[source_bag][source_slot];
        const bool descriptor_valid = descriptor.category > 0 && descriptor.count > 0 &&
            descriptor.payload_size > 0 && descriptor.payload_size <= virtual_bag::kMaxSerializedItem &&
            g_module_objects[source_bag][source_slot] == source_item &&
            g_module_object_categories[source_bag][source_slot] == descriptor.category &&
            g_module_object_hashes[source_bag][source_slot] == virtual_bag::payload_hash(descriptor) &&
            g_module_object_handles[source_bag][source_slot] != 0 && use.generation != 0 &&
            !use.active && !use.pending_release;
        const auto& session = g_extension_drag_session;
        const bool session_valid = g_module_view_installed &&
            g_module_view_index == source_bag && g_projected_item_root != nullptr &&
            valid_child_locked(g_projected_item_root, source_slot) == source_control &&
            session.phase != virtual_bag::DragPhase::kIdle &&
            session.source_bag == source_bag && session.source_slot == source_slot &&
            virtual_bag::drag_session_source_identity_matches(
                session, source_bag, source_slot,
                g_module_object_handles[source_bag][source_slot]) &&
            virtual_bag::drag_session_generation_current(session, g_extension_tab_generation);
        if (!descriptor_valid || !session_valid || backup == nullptr ||
            !module_use_begin_locked(source_bag, source_slot, source_item, &use_token)) {
            VIRTBAG_LOG("equip control blocked caller=UIEquip_EquipControlEventProc source=%p/%d/%d target=%p descriptor=%d session=%d",
                        source_item, source_bag, source_slot, target_item,
                        descriptor_valid ? 1 : 0, session_valid ? 1 : 0);
            cancel_projected_drag_session_locked("equip-control-validation-failed");
            return VirtualBagEquipControlEventResult::kBlocked;
        }
    }

    struct ActiveUseTokenScope {
        ModuleUseToken* previous = nullptr;
        explicit ActiveUseTokenScope(ModuleUseToken* current)
            : previous(g_active_module_use_token) {
            g_active_module_use_token = current;
        }
        ~ActiveUseTokenScope() { g_active_module_use_token = previous; }
    } active_use_token_scope(&use_token);
    const uint64_t original_result = backup(control, event, x2, param);

    bool consumed = false;
    bool finished = false;
    ModuleUseReleasePlan release_plan{};
    {
        std::lock_guard<std::mutex> lock(g_virtual_bag_mtx);
        finished = module_use_finish_locked(use_token, &consumed, &release_plan);
        if (extension_bag_finish_requires_abort(finished)) {
            const bool aborted = module_use_abort_locked(use_token);
            const bool isolated = aborted || module_use_abort_isolate_locked(use_token);
            VIRTBAG_LOG_ERROR("ERROR equip control token finish failed source=%p/%d/%d aborted=%d isolated=%d",
                        source_item, source_bag, source_slot, aborted ? 1 : 0,
                        isolated ? 1 : 0);
            cancel_projected_drag_session_locked("equip-control-finish-mismatch");
        } else if (!consumed) {
            VIRTBAG_LOG("equip control blocked caller=UIEquip_EquipControlEventProc source=%p/%d/%d target=%p finished=%d consumed=%d",
                        source_item, source_bag, source_slot, target_item,
                        finished ? 1 : 0, consumed ? 1 : 0);
            cancel_projected_drag_session_locked("equip-control-not-consumed");
        }
    }
    if (!finished || !consumed) return VirtualBagEquipControlEventResult::kBlocked;

    bool released = true;
    if (release_plan.pending) {
        released = module_use_release_native(release_plan);
        std::lock_guard<std::mutex> lock(g_virtual_bag_mtx);
        if (released) released = module_use_finalize_release_locked(release_plan);
    }
    if (!released) {
        std::lock_guard<std::mutex> lock(g_virtual_bag_mtx);
        cancel_projected_drag_session_locked("equip-control-native-release-failed");
        return VirtualBagEquipControlEventResult::kBlocked;
    }

    bool session_committed = false;
    {
        std::lock_guard<std::mutex> lock(g_virtual_bag_mtx);
        auto& session = g_extension_drag_session;
        if (session.phase == virtual_bag::DragPhase::kPressed) {
            virtual_bag::session_on_native_moving(&session, g_extension_tab_generation);
        }
        if (session.source_bag == source_bag && session.source_slot == source_slot &&
            virtual_bag::drag_session_generation_current(session, g_extension_tab_generation) &&
            session.phase == virtual_bag::DragPhase::kNativeMoving &&
            virtual_bag::session_resolve_target(
                &session, g_extension_tab_generation,
                virtual_bag::DragTargetKind::kEquipmentSlot) != virtual_bag::DragTransition::kIllegal &&
            virtual_bag::session_begin_transaction(
                &session, g_extension_tab_generation) != virtual_bag::DragTransition::kIllegal &&
            virtual_bag::session_claim_transaction(&session) &&
            virtual_bag::session_finish_transaction(&session, true) ==
                virtual_bag::DragTransition::kAdvanced) {
            session_committed = true;
            VIRTBAG_LOG("equip control committed session token=%llu source=%d/%d target_slot=%d",
                        static_cast<unsigned long long>(session.token), source_bag, source_slot,
                        target_slot_index);
        } else {
            cancel_projected_drag_session_locked("equip-control-session-commit-failed");
        }
    }
    if (!session_committed) return VirtualBagEquipControlEventResult::kBlocked;
    VIRTBAG_LOG("equip control handled caller=UIEquip_EquipControlEventProc source=%p/%d/%d target=%p result=%llu",
                source_item, source_bag, source_slot, target_item,
                static_cast<unsigned long long>(original_result));
    if (out_result != nullptr) *out_result = original_result;
    return VirtualBagEquipControlEventResult::kHandled;
}

bool virtual_bag_identify_native_item(void* item, int* out_bag, int* out_slot) {
    if (item == nullptr || out_bag == nullptr || out_slot == nullptr) return false;
    std::lock_guard<std::mutex> lock(g_virtual_bag_mtx);
    return module_slot_of_item_locked(item, out_bag, out_slot);
}

void* virtual_bag_item_at(int bag, int slot) {
    if (!virtual_bag::valid_index(bag) || slot < 0 || slot >= virtual_bag::kSlotCount) {
        return nullptr;
    }
    std::lock_guard<std::mutex> lock(g_virtual_bag_mtx);
    ensure_state_loaded_locked();
    return module_item_locked(bag, slot);
}

void* virtual_bag_view_item_at(int bag, int slot) {
    // 仅当 bag 处于扩展视图（控件 index 与扩展槽 1:1 对应）时才物化兜底；
    // 原版视图下原版坐标读出的 null 就是真空槽，不得误物化成扩展物品。
    if (!g_module_view_installed || g_module_view_index != bag) return nullptr;
    if (!virtual_bag::valid_index(bag) || slot < 0 || slot >= virtual_bag::kSlotCount) {
        return nullptr;
    }
    std::lock_guard<std::mutex> lock(g_virtual_bag_mtx);
    ensure_state_loaded_locked();
    return module_item_locked(bag, slot);
}

void* virtual_bag_find_native_item(int category) {
    if (category <= 0) return nullptr;
    std::lock_guard<std::mutex> lock(g_virtual_bag_mtx);
    ensure_state_loaded_locked();
    for (int bag = 0; bag < virtual_bag::kBagCount; ++bag) {
        for (int slot = 0; slot < g_virtual_bag_state.capacities[bag]; ++slot) {
            const virtual_bag::Item& descriptor = g_virtual_bag_state.items[bag][slot];
            if (descriptor.category != category || descriptor.count <= 0) continue;
            void* item = module_item_locked(bag, slot);
            if (item != nullptr) return item;
            VIRTBAG_LOG("native find extension materialize failed bag=%d slot=%d category=%d",
                        bag, slot, category);
        }
    }
    return nullptr;
}

bool virtual_bag_remove_native_item(void* item, void* raw_use_token) {
    if (item == nullptr) return false;
    std::lock_guard<std::mutex> lock(g_virtual_bag_mtx);
    ensure_state_loaded_locked();
    int bag = -1;
    int slot = -1;
    if (!module_slot_of_item_locked(item, &bag, &slot) ||
        g_module_objects[bag][slot] != item) {
        return false;
    }
    ModuleUseToken* use_token = static_cast<ModuleUseToken*>(raw_use_token);
    if (!module_object_release_allowed_locked(
            bag, slot, item, "native remove", true, use_token)) {
        return false;
    }
    const virtual_bag::Item previous_item = g_virtual_bag_state.items[bag][slot];
    g_virtual_bag_state.items[bag][slot] = {};
    extension_bag_mark_dirty_locked();
    if (!free_module_object_locked(bag, slot, use_token)) {
        g_virtual_bag_state.items[bag][slot] = previous_item;
        VIRTBAG_LOG("native remove rejected object release bag=%d slot=%d", bag, slot);
        return false;
    }
    const bool persisted = persist_state_locked();
    if (g_module_view_installed && g_module_view_index == bag) {
        refresh_module_item_area_locked(bag);
    }
    VIRTBAG_LOG("native remove extension bag=%d slot=%d persisted=%d", bag, slot,
                persisted ? 1 : 0);
    return true;
}

struct SaleReleaseContext {
    qol::contracts::QolItemHandle expected;
};

bool sale_clear_projection(void*, void* control, void* item) {
    if (control == nullptr || extension_bag_ui_item(control) != item) return false;
    return extension_bag_ui_set_control_item(control, nullptr);
}

bool sale_restore_projection(void*, void* control, void* item) {
    if (control == nullptr || item == nullptr) return false;
    const void* current = extension_bag_ui_item(control);
    if (current != nullptr && current != item) return false;
    return extension_bag_ui_set_control_item(control, item);
}

bool sale_release_object(void* raw, void* item, qol::contracts::QolItemHandle expected) {
    auto* context = static_cast<SaleReleaseContext*>(raw);
    if (context == nullptr || item == nullptr || expected.slot != context->expected.slot ||
        expected.generation != context->expected.generation ||
        !extension_bag_sale_item_begin_release(expected, item)) return false;
    const bool freed = extension_free_item(item);
    if (!extension_bag_sale_item_finish_release(expected, freed)) {
        QOL_LOG_ERROR(QolDomain::kExtensionBag,
                      "ERROR sale custody finalize failed slot=%u generation=%u freed=%d",
                      expected.slot, expected.generation, freed ? 1 : 0);
        // A successful pool free is irreversible; report it as committed and leave an
        // ownership diagnostic rather than refunding against a removed object.
        return freed;
    }
    return freed;
}

bool virtual_bag_consume_native_item(void* item, void* raw_use_token) {
    if (item == nullptr) return false;
    void* detail_control = nullptr;
    void* detail_item = nullptr;
    {
        std::lock_guard<std::mutex> lock(g_virtual_bag_mtx);
        ensure_state_loaded_locked();
        int bag = -1;
        int slot = -1;
        if (!module_slot_of_item_locked(item, &bag, &slot) || g_module_objects[bag][slot] != item) {
            return false;
        }
        ModuleUseToken* use_token = static_cast<ModuleUseToken*>(raw_use_token);
        if (!module_object_release_allowed_locked(
                bag, slot, item, "native consume", true,
                use_token)) {
            return false;
        }
        // 消耗前数量取模式视图（R-47 决策 b）：启用态 canonical、关闭态 b，
        // 与 getter 的 observed_before 同域参与扣减判定。
        const int before_count = static_cast<int>(virtual_bag_effective_count(
            item, static_cast<uint32_t>(g_virtual_bag_state.items[bag][slot].count)));
        int observed_before = before_count;
        (void)read_item_cumulate_count(item, &observed_before);
        if (!consume_extension_item_after_native_locked(
                bag, slot, item, before_count, observed_before, "native_consume", use_token)) {
            return false;
        }
        extension_bag_mark_dirty_locked();
        persist_state_locked();
        if (g_module_view_installed && g_module_view_index == bag) {
            refresh_module_item_area_locked(bag);
        }
        if (g_pending_jewel_detail_item != nullptr &&
            virtual_bag::valid_index(g_module_view_index) &&
            g_pending_jewel_detail_bag == g_module_view_index &&
            g_pending_jewel_detail_slot >= 0 &&
            g_pending_jewel_detail_slot < g_virtual_bag_state.capacities[g_module_view_index] &&
            g_module_view_installed && g_projected_item_root != nullptr &&
            g_sdk != nullptr && g_sdk->ui != nullptr) {
            detail_control = valid_child_locked(g_projected_item_root,
                                                g_pending_jewel_detail_slot);
            detail_item = g_pending_jewel_detail_item;
        }
        g_pending_jewel_detail_item = nullptr;
        g_pending_jewel_detail_bag = -1;
        g_pending_jewel_detail_slot = -1;
        VIRTBAG_LOG("native consume extension bag=%d slot=%d before=%d", bag, slot, before_count);
    }
    if (detail_control != nullptr && g_sdk != nullptr && g_sdk->ui != nullptr) {
        (void)extension_bag_ui_set_control_item(detail_control, detail_item);
    }
    if (detail_control != nullptr && g_game != nullptr &&
        g_write_port->ui.make_item_description != nullptr) {
        (void)g_write_port->ui.make_item_description(detail_control);
    }
    return true;
}

void* virtual_bag_current_use_token() {
    return g_active_module_use_token;
}

int virtual_bag_put_jewel_native(void* equip_item, void* jewel_item,
                                 VirtualBagPutJewelBackup backup) {
    if (equip_item == nullptr || jewel_item == nullptr || backup == nullptr) return 3;

    int equip_bag = -1;
    int equip_slot = -1;
    int jewel_bag = -1;
    int jewel_slot = -1;
    {
        std::lock_guard<std::mutex> lock(g_virtual_bag_mtx);
        ensure_state_loaded_locked();
        if (!module_slot_of_item_locked(equip_item, &equip_bag, &equip_slot) ||
            !module_slot_of_item_locked(jewel_item, &jewel_bag, &jewel_slot) ||
            g_module_objects[equip_bag][equip_slot] != equip_item ||
            g_module_objects[jewel_bag][jewel_slot] != jewel_item) {
            return 3;
        }
        if (g_virtual_bag_state.items[jewel_bag][jewel_slot].count <= 0) return 3;
    }

    const int result = backup(equip_item, jewel_item);
    if (result != 0) return result;

    {
        std::lock_guard<std::mutex> lock(g_virtual_bag_mtx);
        ensure_state_loaded_locked();
        if (g_module_objects[equip_bag][equip_slot] != equip_item ||
            g_module_objects[jewel_bag][jewel_slot] != jewel_item) {
            VIRTBAG_LOG("native put jewel payload sync skipped after native success");
            return 0;
        }
        g_pending_jewel_detail_item = equip_item;
        g_pending_jewel_detail_bag = equip_bag;
        g_pending_jewel_detail_slot = equip_slot;
        std::array<uint8_t, virtual_bag::kSerializedItemBuffer> payload{};
        int payload_size = 0;
        if (!serialize_item_payload_locked(equip_item, &payload, &payload_size)) {
            VIRTBAG_LOG("native put jewel payload sync failed bag=%d slot=%d", equip_bag, equip_slot);
            return 0;
        }
        virtual_bag::Item& equip_descriptor = g_virtual_bag_state.items[equip_bag][equip_slot];
        equip_descriptor.payload = payload;
        equip_descriptor.payload_size = payload_size;
        // descriptor.count 恒为 canonical（R-38/R-48；getter 关闭态只返回 b 视图）。
        int equip_count = 0;
        if (read_item_cumulate_count(equip_item, &equip_count)) {
            equip_descriptor.count = static_cast<int>(virtual_bag_canonical_count(equip_item, 1));
        }
        g_module_object_hashes[equip_bag][equip_slot] = virtual_bag::payload_hash(equip_descriptor);
        extension_bag_mark_dirty_locked();
        persist_state_locked();
    }
    return 0;
}

#include "plugins/extension_bag/extension_bag_public_runtime.inc"
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
           ",\"save_panel_hook\":" +
           std::string(g_save_panel_hook_installed ? "true" : "false") +
           ",\"extension_tab_button\":" +
           std::string(g_extension_tab_buttons[0] != nullptr ? "true" : "false") +
           ",\"inventory_frame_active\":" +
           std::string(g_inventory_frame_active ? "true" : "false") +
           ",\"recovery_action\":\"" + extension_recovery_action_name_locked() + "\"" +
           ",\"state\":" + virtual_bag::state_json(g_virtual_bag_state, true) + "}";
}

std::string extension_bag_not_ready_error_locked() {
    if (!g_virtual_bag_enabled.load()) return op_err("extension bag disabled");
    return op_err("not in game");
}

bool extension_bag_ready_locked() {
    // 操作准入与扫描同门：扩展开关 + world + 当前 epoch 已加载（未加载/降级/epoch 未登记
    // 时拒绝物品操作，不惰性 IO 兜底）。session 状态由 provider 回调维护。
    return g_virtual_bag_enabled.load() && read_world_in_world() &&
           extension_bag_session_admitted_locked();
}

int extension_internal_bag(int logical_bag) {
    return virtual_bag::extension_internal_bag(logical_bag);
}

std::string extension_bag_view_result_json(bool ok, const char* error) {
    if (!ok) return op_err(error);
    return "{\"ok\":true,\"state\":" + extension_bag_status_json_locked() + "}";
}

enum class ExtensionBagUnequipResult {
    kOk,
    kNotEquipped,
    kNotEmpty,
    kNoSpace,
    kPersistFailed,
    kFailed,
};

// P3 袋解除（对齐原版 UIEquip_ButtonUnequipExe desc_type=1 语义）：
// 非空 → kNotEmpty（弹窗 7，b8084）；原版袋 0..4 全满 → kNoSpace（弹窗 6，
// b809c）；成功 = 按袋类型重建 count-1 背包物品入库（源袋优先、显式跳过
// 任务袋 5/ADR-006）→ 清装备态 → 切到接收袋（原版 b804c-b805c 双字节写）。
// 顺序铁律：先预检空袋再建对象（原版同序 b7f94→b7fa8），否则非空路径会把
// 已入库物品滞留 g_inven 造成复制；转移成功后仅 persist 失败需回滚
// （INVEN_RemoveItemDirect 返回值不可信 → 槽位重读确认后再真释放）。
ExtensionBagUnequipResult unequip_extension_bag_locked(std::unique_lock<std::mutex>& lock,
                                                       int internal_bag) {
    ensure_state_loaded_locked();
    if (g_virtual_bag_state.capacities[internal_bag] == 0) {
        return ExtensionBagUnequipResult::kNotEquipped;
    }
    for (const virtual_bag::Item& item : g_virtual_bag_state.items[internal_bag]) {
        if (item.category > 0 || item.count > 0) {
            return ExtensionBagUnequipResult::kNotEmpty;
        }
    }
    if (g_game == nullptr || g_write_port->inventory.create_item == nullptr ||
        g_write_port->inventory.save_item_on_empty == nullptr) {
        VIRTBAG_LOG("extension unequip symbols unavailable bag=%d", internal_bag);
        return ExtensionBagUnequipResult::kFailed;
    }
    void* item = nullptr;
    if (!qol::contracts::succeeded(g_write_port->inventory.create_item(
            static_cast<int32_t>(g_virtual_bag_state.types[internal_bag]), 0, 0, 0, &item)) ||
        item == nullptr) {
        VIRTBAG_LOG("extension unequip create item failed bag=%d", internal_bag);
        return ExtensionBagUnequipResult::kFailed;
    }
    // P4.3：全新对象立即入账本；分配失败立即真释放（尚未暴露，安全）。
    uint32_t item_handle = 0;
    if (ownership::allocate(&g_ownership_ledger, &item_handle) != ownership::Outcome::kOk) {
        release_temporary_item_locked(item);
        VIRTBAG_LOG("extension unequip ledger exhausted bag=%d", internal_bag);
        return ExtensionBagUnequipResult::kFailed;
    }
    // 袋对象 +0x10 的 bit0..24 是容量，ITEMSYSTEM_CreateItem（0x10be9c）已按
    // ITEMSTATICBASE[category]（1→4、2→8、3→12、4→16）自动写入。只改
    // bit25..31，native bag object 只写 marker；禁止先做 S2 编码再叠加 marker，
    // 因为当前 marker mask 与 S2 B 段重叠。
    {
        uint32_t flags = 0;
        const auto* count_api = item_count_api();
        if (count_api == nullptr || count_api->read_encoded_field == nullptr ||
            count_api->write_encoded_field == nullptr ||
            !qol::contracts::succeeded(count_api->read_encoded_field(item, &flags)) ||
            !qol::contracts::succeeded(count_api->write_encoded_field(
                item, ::qol::features::extension_bag::write_native_bag_object_marker(flags)))) {
            release_temporary_item_locked(item);
            VIRTBAG_LOG("extension unequip bag marker write failed bag=%d", internal_bag);
            return ExtensionBagUnequipResult::kFailed;
        }
    }

    int preferred = original_bag_locked();
    if (preferred < 0 || preferred > 4) preferred = g_virtual_bag_state.original_selected;
    if (preferred < 0 || preferred > 4) preferred = 0;
    // 排除 internal_bag 自身：正在解除的袋不得作为接收方——袋对象若放回
    // 自己的行/槽，随后 unequip_bag 清袋会把刚放回的物品一起清掉
    // （真机实证：extension unequip adopt bag=N dst=N/0 后物品丢失/解除失败）。
    int order[5];
    int order_count = 0;
    for (int bag = 0; bag < 5; ++bag) {
        if (bag == internal_bag || bag == preferred) continue;
        order[order_count++] = bag;
    }
    if (preferred != internal_bag) {
        for (int i = order_count; i > 0; --i) order[i] = order[i - 1];
        order[0] = preferred;
        ++order_count;
    }
    int receiving_bag = -1;
    for (int i = 0; i < order_count && receiving_bag < 0; ++i) {
        int save_result = 0;
        if (qol::contracts::succeeded(g_write_port->inventory.save_item_on_empty(
                item, order[i], &save_result)) && save_result != 0) {
            receiving_bag = order[i];
        }
    }
    if (receiving_bag < 0) {
        // 原版袋 0..4 全满：袋对象不回原版背包，转而收编为扩展袋物品槽的
        // 普通袋物品（与装备卸下收编 virtual_bag_adopt_unequipped_item 同
        // 模式）。收编后用户可把它拖回原版背包，或拖到空扩展标签重新装备。
        int adopt_bag = -1;
        int adopt_slot = -1;
        for (int bag = 0; bag < virtual_bag::kBagCount && adopt_bag < 0; ++bag) {
            if (bag == internal_bag) continue;  // 排除正在解除的袋（见上）
            if (g_virtual_bag_state.types[bag] == 0) continue;
            const int capacity = g_virtual_bag_state.capacities[bag];
            for (int slot = 0; slot < capacity; ++slot) {
                if (module_slot_is_assignable_locked(bag, slot)) {
                    adopt_bag = bag;
                    adopt_slot = slot;
                    break;
                }
            }
        }
        if (adopt_bag < 0) {
            // 原版背包与所有扩展袋物品槽都满：真无去处，释放并保持原版弹窗。
            release_tracked_item_locked(item_handle, item, "unequip no space");
            return ExtensionBagUnequipResult::kNoSpace;
        }
        std::array<uint8_t, virtual_bag::kSerializedItemBuffer> payload{};
        int payload_size = 0;
        if (!serialize_item_payload_locked(item, &payload, &payload_size)) {
            release_tracked_item_locked(item_handle, item, "unequip adopt serialize");
            return ExtensionBagUnequipResult::kFailed;
        }
        virtual_bag::Item& descriptor = g_virtual_bag_state.items[adopt_bag][adopt_slot];
        descriptor.payload = payload;
        descriptor.payload_size = payload_size;
        (void)read_item_category(item, &descriptor.category);
        // 袋对象 count 恒为 1（本路径只处理袋物品）。不能用
        // fn_get_cumulate_count：CreateItem 后 +0x10 高位 count 区为 0，
        // 读得 0 会让收编袋显示为空槽（下次收编仍会选中它）。
        descriptor.count = 1;
        // 对象移交目标扩展槽保管（item_handle 早前已 allocate，沿用）。
        if (!module_object_generation_advance_locked(adopt_bag, adopt_slot, "unequip adopt")) {
            release_tracked_item_locked(item_handle, item, "unequip adopt slot protected");
            return ExtensionBagUnequipResult::kFailed;
        }
        if (!extension_bag_rebind_item_handle(item)) {
            release_tracked_item_locked(item_handle, item, "unequip adopt handle registration");
            return ExtensionBagUnequipResult::kFailed;
        }
        g_module_objects[adopt_bag][adopt_slot] = item;
        g_module_object_categories[adopt_bag][adopt_slot] = descriptor.category;
        g_module_object_hashes[adopt_bag][adopt_slot] = virtual_bag::payload_hash(descriptor);
        g_module_object_handles[adopt_bag][adopt_slot] = item_handle;
        const uint8_t adopted_type = g_virtual_bag_state.types[internal_bag];
        virtual_bag::unequip_bag(&g_virtual_bag_state, internal_bag);
        extension_bag_mark_dirty_locked();
        if (persist_state_locked()) {
            if (g_module_view_installed && g_module_view_index == adopt_bag) {
                refresh_module_item_area_locked(adopt_bag);
            }
            VIRTBAG_LOG("extension unequip adopt bag=%d type=%d dst=%d/%d", internal_bag,
                        static_cast<int>(adopted_type), adopt_bag, adopt_slot);
            return ExtensionBagUnequipResult::kOk;
        }
        // persist 失败：回滚收编与袋装备态，对象真释放。
        if (!module_object_generation_advance_locked(adopt_bag, adopt_slot, "unequip rollback")) {
            VIRTBAG_LOG("extension unequip rollback blocked by active object bag=%d slot=%d",
                        adopt_bag, adopt_slot);
            return ExtensionBagUnequipResult::kPersistFailed;
        }
        g_module_objects[adopt_bag][adopt_slot] = nullptr;
        g_module_object_categories[adopt_bag][adopt_slot] = 0;
        g_module_object_hashes[adopt_bag][adopt_slot] = 0;
        g_module_object_handles[adopt_bag][adopt_slot] = 0;
        descriptor = virtual_bag::Item{};
        g_virtual_bag_state.types[internal_bag] = adopted_type;
        g_virtual_bag_state.isolation_now_ms = isolation_now_ms_locked();
        virtual_bag::normalize(&g_virtual_bag_state);
        release_tracked_item_locked(item_handle, item, "unequip adopt rollback");
        persist_state_locked();
        VIRTBAG_LOG("extension unequip adopt persist failed bag=%d dst=%d/%d", internal_bag,
                    adopt_bag, adopt_slot);
        return ExtensionBagUnequipResult::kPersistFailed;
    }

    // 入库成功后 g_inven 拥有对象，模块不得再释放；预检后 unequip_bag 不会
    // 因非空失败。UI 恢复仍由 virtual_bag_draw_end_wrapper 下一帧完成。
    const uint8_t saved_type = g_virtual_bag_state.types[internal_bag];
    virtual_bag::unequip_bag(&g_virtual_bag_state, internal_bag);
    virtual_bag::enter_original(&g_virtual_bag_state, receiving_bag);
    set_original_bag_locked(receiving_bag);
    extension_bag_mark_dirty_locked();
    if (persist_state_locked()) {
        // P4.3：persist 成功 = 提交点，此刻才移交原版库存（终态）。
        handover_tracked_item_locked(item_handle, item, "unequip handover");
        VIRTBAG_LOG("extension unequip ok bag=%d type=%d receiving=%d", internal_bag,
                    static_cast<int>(saved_type), receiving_bag);
        return ExtensionBagUnequipResult::kOk;
    }
    bool rolled_back = false;
    for (int slot = 0; slot < virtual_bag::kSlotCount; ++slot) {
            void* slot_item = nullptr;
            if (!inventory_slot_locked(receiving_bag, slot, &slot_item) ||
                slot_item != item) {
                continue;
            }
            if (remove_item_direct_unlocked(lock, receiving_bag, slot, item,
                                             "unequip rollback")) {
                rolled_back = true;
            }
            break;
        }
    if (rolled_back) {
        // P4.3：回滚已确认（脱离 INVEN）→ 账本终止保管 + 真释放。
        release_tracked_item_locked(item_handle, item, "unequip rollback");
        g_virtual_bag_state.types[internal_bag] = saved_type;
        g_virtual_bag_state.isolation_now_ms = isolation_now_ms_locked();
        virtual_bag::normalize(&g_virtual_bag_state);
        persist_state_locked();
        VIRTBAG_LOG("extension unequip persist failed; rolled back bag=%d", internal_bag);
        return ExtensionBagUnequipResult::kPersistFailed;
    }
    // 回滚未确认：对象去向不明，按 handover 记账（inventory-owned 终态，审计可见）。
    handover_tracked_item_locked(item_handle, item, "unequip rollback uncertain");
    VIRTBAG_LOG("extension unequip rollback failed bag=%d receiving=%d", internal_bag,
                receiving_bag);
    return ExtensionBagUnequipResult::kFailed;
}

void show_extension_bag_not_empty_popup() {
    if (g_sdk != nullptr && g_sdk->ui != nullptr && g_sdk->ui->show_text_popup != nullptr) {
        (void)g_sdk->ui->show_text_popup(7);
    }
}

void show_extension_bag_no_space_popup() {
    if (g_sdk != nullptr && g_sdk->ui != nullptr && g_sdk->ui->show_text_popup != nullptr) {
        (void)g_sdk->ui->show_text_popup(6);
    }
}

// P3 真实装备（原版两装备入口的扩展对应物；拖放路径经扩展标签 drop，
// 装备按钮路径经 desc 按钮 hook 溢出）。事务顺序对齐解除侧的反向操作：
// 校验 → INVEN 定位源槽 → payload 备份 → RemoveItemDirect+重读确认
// （返回值不可信，control-plane v1.7）→ equip_bag 占位（容量由 normalize
// 派生）→ persist；失败回滚（清位 + 按备份 payload 重建入库）。
// P4.3：源物品在移除确认后入账本保管，提交/回滚均经 retire 终止保管
// （触摸窗口外真释放）；回滚重建对象经 tracked load + handover/release。
#include "plugins/extension_bag/extension_bag_equip.inc"
#include "plugins/extension_bag/extension_bag_store.inc"
#include "plugins/extension_bag/extension_bag_mix.inc"

}  // namespace

QolResult virtual_bag_remove_for_sale(int32_t logical_bag, int32_t slot,
                                      qol::contracts::QolItemHandle expected_item) {
    const int bag = virtual_bag::extension_internal_bag(logical_bag);
    if (!virtual_bag::valid_index(bag) || slot < 0 || slot >= virtual_bag::kSlotCount) {
        return qol::contracts::error(kQolErrInvalidArgument, "invalid extension sale slot", 27);
    }
    std::lock_guard<std::mutex> lock(g_virtual_bag_mtx);
    if (!g_virtual_bag_enabled.load(std::memory_order_acquire) ||
        !extension_bag_session_admitted_locked()) {
        return qol::contracts::error(kQolErrNotReady, "extension sale session unavailable", 34);
    }
    if (object_touch_window_active_locked()) {
        return qol::contracts::error(kQolErrBusy, "extension sale during touch window", 34);
    }

    virtual_bag::Item& descriptor = g_virtual_bag_state.items[bag][slot];
    void* const item = g_module_objects[bag][slot];
    int item_bag = -1;
    int item_slot = -1;
    qol::contracts::QolItemHandle current_item{};
    const bool located = item != nullptr && module_slot_of_item_locked(item, &item_bag, &item_slot) &&
                         item_bag == bag && item_slot == slot;
    const bool custody_matches = located &&
        extension_bag_sale_item_matches(expected_item, item, &current_item);
    const bool descriptor_match = located && descriptor.category > 0 && descriptor.count > 0 &&
                                  g_module_object_categories[bag][slot] == descriptor.category;
    const bool hash_matches = descriptor_match &&
        g_module_object_hashes[bag][slot] == virtual_bag::payload_hash(descriptor);
    const bool release_allowed = located &&
        module_object_release_allowed_locked(bag, slot, item, "inventory sale", false, nullptr);
    if (!descriptor_match || !hash_matches || !custody_matches || !release_allowed) {
        return qol::contracts::error(kQolErrConflict, "extension sale identity stale", 29);
    }
    void* projection_control = nullptr;
    if (g_module_view_installed && g_module_view_index == bag) {
        if (g_projected_item_root == nullptr ||
            (projection_control = valid_child_locked(g_projected_item_root, slot)) == nullptr ||
            extension_bag_ui_item(projection_control) != item) {
            return qol::contracts::error(kQolErrConflict, "extension sale projection stale", 31);
        }
    }

    ModuleUseState& use = g_module_object_use[bag][slot];
    const int capacity = g_virtual_bag_state.capacities[bag];
    qol::features::extension_bag::model::SaleSlotState state{};
    state.logical_bag = logical_bag;
    state.slot = slot;
    state.capacity = capacity;
    state.session_admitted = extension_bag_session_admitted_locked();
    state.descriptor_matches_object = located && g_module_objects[bag][slot] == item;
    state.descriptor_hash_matches = hash_matches;
    state.handle_custody_matches = custody_matches;
    state.expected_item = expected_item;
    state.current_item = current_item;
    state.descriptor = &descriptor;
    state.object = &g_module_objects[bag][slot];
    state.object_category = &g_module_object_categories[bag][slot];
    state.object_hash = &g_module_object_hashes[bag][slot];
    state.object_handle = &g_module_object_handles[bag][slot];
    state.ledger_handle = g_module_object_handles[bag][slot];
    state.ledger = &g_ownership_ledger;
    state.use_active = &use.active;
    state.use_consumed = &use.consumed;
    state.use_pending_release = &use.pending_release;
    state.use_item = &use.item;
    state.use_generation = &use.generation;
    state.use_owner = &use.owner;
    state.dirty = &g_item_state_dirty;
    state.revision = &g_item_state_revision;
    state.projection_control = projection_control;
    SaleReleaseContext release_context{expected_item};
    const qol::features::extension_bag::model::SaleRemovalOps ops{
        &release_context, &sale_clear_projection,
        &sale_restore_projection, &sale_release_object};
    const auto removed = qol::features::extension_bag::model::remove_for_sale(state, ops);
    if (removed != qol::features::extension_bag::model::SaleRemovalResult::kRemoved &&
        removed != qol::features::extension_bag::model::SaleRemovalResult::kRemovedLedgerDiagnostic) {
        const int error = removed == qol::features::extension_bag::model::SaleRemovalResult::kNotAdmitted
                              ? kQolErrNotReady
                          : removed == qol::features::extension_bag::model::SaleRemovalResult::kActiveUse
                              ? kQolErrBusy
                              : kQolErrConflict;
        QOL_LOG_WARN(QolDomain::kExtensionBag,
                     "extension sale removal rejected logical_bag=%d slot=%d result=%u",
                     logical_bag, slot, static_cast<unsigned>(removed));
        return qol::contracts::error(error, "extension sale removal rejected", 31);
    }
    if (removed == qol::features::extension_bag::model::SaleRemovalResult::kRemovedLedgerDiagnostic) {
        QOL_LOG_ERROR(QolDomain::kExtensionBag,
                      "ERROR sale removed with ownership ledger diagnostic logical_bag=%d slot=%d",
                      logical_bag, slot);
    }
    if (g_extension_desc_item == item) {
        reset_extension_desc_item_hooks_locked();
        g_extension_desc_item = nullptr;
        g_extension_desc_item_bag = -1;
        g_extension_desc_item_slot = -1;
        clear_original_desc_locked();
    }
    QOL_LOG_INFO(QolDomain::kExtensionBag, "extension sale removed logical_bag=%d slot=%d",
                 logical_bag, slot);
    return qol::contracts::ok();
}

bool virtual_bag_handle_confirm_use_item(void* item) {
    return extension_confirm_use_item(item);
}

bool virtual_bag_handle_use_mercenary_seal() {
    return extension_use_mercenary_seal();
}

// 商店扩展视图会把窗口原版袋容量字临时放大到扩展容量；若玩家在此视图下
// 触发原版库存写入（买入→INVEN_SaveItem），FindSaveSlot 会按放大容量找
// 空槽，可能把物品写进超过真实容量的槽位（恢复后物品丢失/错乱）。该函数
// 由 SaveItem hook wrapper 在 backup 前调用：商店投影激活时先恢复投影
// （容量字还原），让原版按真实容量入库。
void virtual_bag_store_restore_for_original_write() {
    std::lock_guard<std::mutex> lock(g_virtual_bag_mtx);
    if (store_view_installed) {
        VIRTBAG_LOG("store save gate: restore store module view before original write");
        store_restore_module_view_locked();
    }
}

bool virtual_bag_equip_projected_item(void* character, void* item, int source_bag, int source_slot,
                                      int equip_slot) {
    if (character == nullptr || item == nullptr) return false;
    std::unique_lock<std::mutex> lock(g_virtual_bag_mtx);
    if (!extension_bag_session_admitted_locked()) return false;
    if (!g_module_view_installed || g_module_view_index != source_bag) return false;
    if (g_game == nullptr || g_character_extras == nullptr || g_character_extras->read.find_equip_slot == nullptr) return false;
    int found = -1;
    if (!qol::contracts::succeeded(
            g_character_extras->read.find_equip_slot(character, item, &found))) {
        return false;
    }
    if (found < 0 || found >= kEquipSlotCount) return false;
    // 原版在源槽读空时传 equip_slot=-1（FindEquipSlot(null) 的结果）；扩展
    // 物品不在 INVEN，不能把 -1 当校验失败，以真实物品计算的装备槽为准。
    if (equip_slot >= 0 && equip_slot < kEquipSlotCount && equip_slot != found) return false;
    return equip_module_item_on_character_locked(lock, character, item, source_bag, source_slot);
}

bool virtual_bag_has_empty_slots(int needed, int include_task_bag) {
    if (needed <= 0) return true;
    std::lock_guard<std::mutex> lock(g_virtual_bag_mtx);
    ensure_state_loaded_locked();
    int empty = 0;
    const int bag_count = include_task_bag != 0 ? virtual_bag::kBagCount : virtual_bag::kOriginalTransactionBagCount;
    for (int bag = 0; bag < bag_count; ++bag) {
        const int capacity = g_virtual_bag_state.capacities[bag];
        for (int slot = 0; slot < capacity && slot < virtual_bag::kSlotCount; ++slot) {
            if (module_slot_is_assignable_locked(bag, slot)) {
                ++empty;
                if (empty >= needed) return true;
            }
        }
    }
    return false;
}

bool virtual_bag_adopt_unequipped_item(void* character, int equip_slot) {
    if (character == nullptr || equip_slot < 0 || equip_slot >= kEquipSlotCount) return false;
    std::lock_guard<std::mutex> lock(g_virtual_bag_mtx);
    if (!extension_bag_session_admitted_locked()) return false;
    if (g_game == nullptr || g_character_extras == nullptr || g_character_extras->read.equipped_item == nullptr) return false;
    void* item = nullptr;
    if (!qol::contracts::succeeded(
            g_character_extras->read.equipped_item(character, equip_slot, &item))) {
        return false;
    }
    if (item == nullptr) return false;
    int existing_bag = -1;
    int existing_slot = -1;
    if (module_slot_of_item_locked(item, &existing_bag, &existing_slot)) return false;
    std::array<uint8_t, virtual_bag::kSerializedItemBuffer> payload{};
    int payload_size = 0;
    if (!serialize_item_payload_locked(item, &payload, &payload_size)) return false;
    int dst_bag = -1;
    int dst_slot = -1;
    for (int bag = 0; bag < virtual_bag::kBagCount && dst_bag < 0; ++bag) {
        if (g_virtual_bag_state.types[bag] == 0) continue;
        const int capacity = g_virtual_bag_state.capacities[bag];
        for (int slot = 0; slot < capacity; ++slot) {
            if (module_slot_is_assignable_locked(bag, slot)) {
                dst_bag = bag;
                dst_slot = slot;
                break;
            }
        }
    }
    if (dst_bag < 0) return false;
    virtual_bag::Item& descriptor = g_virtual_bag_state.items[dst_bag][dst_slot];
    descriptor.payload = payload;
    descriptor.payload_size = payload_size;
    (void)read_item_category(item, &descriptor.category);
    // descriptor.count 恒为 canonical（R-38/R-48；getter 关闭态只返回 b 视图）：
    // 卸下收编的物品数量位保留完整 128a+b。
    descriptor.count = 1;
    int item_count = 0;
    if (read_item_cumulate_count(item, &item_count)) {
        descriptor.count = static_cast<int>(virtual_bag_canonical_count(item, 1));
    }
    // 装备槽对象仍被原版 UI（装备槽 desc/控件缓存）引用，不能释放给对象池；
    // 直接收编为扩展槽的物化对象（与 equip_module_item_on_character_locked
    // 对旧装备的收编同规则）。
    if (!module_object_generation_advance_locked(dst_bag, dst_slot, "unequip replacement")) {
        VIRTBAG_LOG("native unequip adopt blocked by active object bag=%d slot=%d",
                    dst_bag, dst_slot);
        return false;
    }
    g_module_objects[dst_bag][dst_slot] = item;
    g_module_object_categories[dst_bag][dst_slot] = descriptor.category;
    g_module_object_hashes[dst_bag][dst_slot] = virtual_bag::payload_hash(descriptor);
    uint32_t adopted_handle = 0;
    if (ownership::allocate(&g_ownership_ledger, &adopted_handle) == ownership::Outcome::kOk) {
        g_module_object_handles[dst_bag][dst_slot] = adopted_handle;
    } else {
        g_module_object_handles[dst_bag][dst_slot] = 0;
        VIRTBAG_LOG("extension unequip adopt ledger exhausted bag=%d slot=%d",
                    dst_bag, dst_slot);
    }
    if (g_game->character_write.set_equipped_item == nullptr ||
        !qol::contracts::succeeded(
            g_game->character_write.set_equipped_item(character, equip_slot, nullptr))) {
        return false;
    }
    if (!extension_bag_rebind_item_handle(item)) {
        VIRTBAG_LOG("native unequip adopt handle registration failed bag=%d slot=%d",
                    dst_bag, dst_slot);
        return false;
    }
    extension_bag_mark_dirty_locked();
    persist_state_locked();
    if (g_module_view_installed && g_module_view_index == dst_bag) {
        refresh_module_item_area_locked(dst_bag);
    }
    VIRTBAG_LOG("native unequip adopt equip_slot=%d bag=%d slot=%d",
                equip_slot, dst_bag, dst_slot);
    return true;
}

// ---- 入库落位单一 owner（R-53）----
// SaveItem 漏斗的两条入口（backup 前合并 / backup 失败后收编）共用同一落位驱动：
// 先并入扩展袋既有同类堆（adopt_merge_into 判据），否则（仅 allow_new_slot）新建
// 扩展空槽。owner 内部取 g_virtual_bag_mtx，只做副作用（对象写回/descriptor/hash/
// persist/refresh/free），不调用被 Hook 原版（仅 defer_item_free_locked/
// fn_itempool_free），满足 R-44。original-first 由调用方 wrapper 保证：本 owner
// 不触碰原版 backup。
enum class VirtualBagPlaceOutcome : uint8_t {
    kNotApplicable,  // null/禁用/门控不符/已在模块槽：调用方保持原版
    kMerged,         // 并入既有扩展同类堆
    kPlacedNewSlot,  // 新建扩展槽（仅 allow_new_slot）
    kNoSpace,        // 无合并目标且无空槽
    kRejected,       // serialize/generation 拒绝（失败路径已回滚）
};

struct VirtualBagPlaceRequest {
    void* item = nullptr;
    bool allow_new_slot = false;  // merge=false, adopt=true
    bool require_world = false;   // merge=true, adopt=false
};

VirtualBagPlaceOutcome virtual_bag_place_native_item(const VirtualBagPlaceRequest& req) {
    if (req.item == nullptr || !g_virtual_bag_enabled.load()) {
        return VirtualBagPlaceOutcome::kNotApplicable;
    }
    if (req.require_world && !read_world_in_world()) {
        return VirtualBagPlaceOutcome::kNotApplicable;
    }
    std::lock_guard<std::mutex> lock(g_virtual_bag_mtx);
    if (!extension_bag_session_admitted_locked()) return VirtualBagPlaceOutcome::kNotApplicable;
    int holder_bag = -1;
    int holder_slot = -1;
    if (module_slot_of_item_locked(req.item, &holder_bag, &holder_slot)) {
        return VirtualBagPlaceOutcome::kNotApplicable;
    }
    std::array<uint8_t, virtual_bag::kSerializedItemBuffer> payload{};
    int payload_size = 0;
    if (!serialize_item_payload_locked(req.item, &payload, &payload_size)) {
        return VirtualBagPlaceOutcome::kRejected;
    }
    int category = 0;
    (void)read_item_category(req.item, &category);
    // descriptor.count 恒为 canonical（R-38/R-48）：getter 关闭态只返回 b 视图，
    // 必须按 S2 解码保存完整 128a+b，否则 sidecar 读档会把 a 段对齐抹掉。
    int canonical_count = static_cast<int>(virtual_bag_canonical_count(req.item, 1));
    if (canonical_count <= 0) canonical_count = 1;
    const bool limit_enabled = virtual_bag_extended_mode(req.item);
    virtual_bag::Item source{};
    source.category = category;
    source.count = canonical_count;
    source.payload = payload;
    source.payload_size = payload_size;

    const virtual_bag::PlacePlan plan = virtual_bag::place_plan(
        g_virtual_bag_state.items, g_virtual_bag_state.types,
        g_virtual_bag_state.capacities, source, req.allow_new_slot, limit_enabled,
        virtual_bag_category_uses_stack_count, &module_slot_is_assignable_locked);

    if (plan.outcome == virtual_bag::PlaceOutcome::kMerged) {
        const int bag = plan.bag;
        const int slot = plan.slot;
        const virtual_bag::Item& merged = plan.merged;
        // 物化对象同步：已有堆对象（若已物化）数量位按模式视图写回，保持
        // 对象 +0x10 与 descriptor canonical 一致（非 count-encoded 在
        // adopt_merge_into 的 mergeable_items 门控内已拒绝，R-46）。
        void* materialized = g_module_objects[bag][slot];
        if (materialized != nullptr) {
            const uint32_t merged_view = virtual_bag_view_from_canonical(
                static_cast<uint32_t>(merged.count), virtual_bag_extended_mode(materialized));
            (void)virtual_bag_write_effective(materialized, merged_view);
        }
        g_virtual_bag_state.items[bag][slot] = merged;
        g_module_object_hashes[bag][slot] = virtual_bag::payload_hash(merged);
        extension_bag_mark_dirty_locked();
        persist_state_locked();
        if (g_module_view_installed && g_module_view_index == bag) {
            refresh_module_item_area_locked(bag);
        }
        // 收编对象未进库、无其他持有者（调用方前置门控），并入后释放回池。
        if (!defer_item_free_locked(req.item)) {
            if (g_game != nullptr && g_write_port->inventory.free_item != nullptr) {
                (void)g_write_port->inventory.free_item(req.item);
            }
        }
        VIRTBAG_LOG("native save place merge bag=%d slot=%d category=%d count=%d->%d item=%p",
                    bag, slot, category, canonical_count, merged.count, req.item);
        return VirtualBagPlaceOutcome::kMerged;
    }
    if (plan.outcome == virtual_bag::PlaceOutcome::kPlacedNewSlot) {
        const int bag = plan.bag;
        const int slot = plan.slot;
        virtual_bag::Item& descriptor = g_virtual_bag_state.items[bag][slot];
        const virtual_bag::Item previous_descriptor = descriptor;
        descriptor = virtual_bag::apply_new_slot_descriptor(previous_descriptor, source, true);
        // 原版对象直接收编为扩展槽物化对象（SaveItem 失败对象未进库、无其他
        // 持有者；落位后生命周期归模块 g_module_objects 管理）。generation 校验
        // 失败必须回滚 descriptor，避免 descriptor 与 g_module_objects 分叉。
        if (!module_object_generation_advance_locked(bag, slot, "native save place")) {
            descriptor =
                virtual_bag::apply_new_slot_descriptor(previous_descriptor, source, false);
            VIRTBAG_LOG("native save place blocked by active object bag=%d slot=%d", bag, slot);
            return VirtualBagPlaceOutcome::kRejected;
        }
        if (!extension_bag_rebind_item_handle(req.item)) {
            descriptor =
                virtual_bag::apply_new_slot_descriptor(previous_descriptor, source, false);
            VIRTBAG_LOG("native save place handle registration failed bag=%d slot=%d", bag, slot);
            return VirtualBagPlaceOutcome::kRejected;
        }
        g_module_objects[bag][slot] = req.item;
        g_module_object_categories[bag][slot] = descriptor.category;
        g_module_object_hashes[bag][slot] = virtual_bag::payload_hash(descriptor);
        uint32_t adopted_handle = 0;
        if (ownership::allocate(&g_ownership_ledger, &adopted_handle) ==
            ownership::Outcome::kOk) {
            g_module_object_handles[bag][slot] = adopted_handle;
        } else {
            g_module_object_handles[bag][slot] = 0;
            VIRTBAG_LOG("native save place ledger exhausted bag=%d slot=%d", bag, slot);
        }
        extension_bag_mark_dirty_locked();
        persist_state_locked();
        if (g_module_view_installed && g_module_view_index == bag) {
            refresh_module_item_area_locked(bag);
        }
        VIRTBAG_LOG("native save place new bag=%d slot=%d item=%p category=%d count=%d",
                    bag, slot, req.item, descriptor.category, descriptor.count);
        return VirtualBagPlaceOutcome::kPlacedNewSlot;
    }
    return plan.outcome == virtual_bag::PlaceOutcome::kRejected
               ? VirtualBagPlaceOutcome::kRejected
               : VirtualBagPlaceOutcome::kNoSpace;
}

bool virtual_bag_merge_native_item(void* item) {
    // INVEN_SaveItem backup 前的扩展同类堆合并（VM-39 拾取并入扩展堆）：
    // original-first——仅当扩展袋已有可合并堆（allow_new_slot=false）时并入并
    // 返回 true；找不到则返回 false 让原版按真实容量入库。薄包装 owner。
    const VirtualBagPlaceRequest req{item, /*allow_new_slot=*/false, /*require_world=*/true};
    return virtual_bag_place_native_item(req) == VirtualBagPlaceOutcome::kMerged;
}

bool virtual_bag_adopt_native_item(void* item) {
    // INVEN_SaveItem（0x104528）无空位（FindSaveSlot 失败）时的扩展袋接管：
    // 先并入既有同类堆，否则新建扩展空槽（allow_new_slot=true）。返回 true 则
    // wrapper 上报成功，上层（任务奖励/事件发奖/开箱/拾取等）不会把物品掉地或
    // 静默释放。薄包装 owner。
    const VirtualBagPlaceRequest req{item, /*allow_new_slot=*/true, /*require_world=*/false};
    const VirtualBagPlaceOutcome outcome = virtual_bag_place_native_item(req);
    return outcome == VirtualBagPlaceOutcome::kMerged ||
           outcome == VirtualBagPlaceOutcome::kPlacedNewSlot;
}

// ---- Path A'：装备按钮函数级接管（Stage4 第 10 hook 的扩展侧实现）----
// UIEquip_ButtonEquipExe（0xb7c18）被函数级 hook 后，wrapper 先进这里判定。
// 只接管"详情物品来自扩展槽的背包物品"：原版 BagEquipExe 的删源是内联
// 指令（b7e04 str xzr 写 INVEN[袋][槽]），扩展物品不在 INVEN，走原函数会
// 删空槽导致源不销毁 + 袋对象被袋表与 g_module_objects 双持有（点击崩
// 溃）。因此：
//   原版有空袋 → 扩展出库 + 袋表写入（b7dbc 同款：bag_table[bag]=item，
//                袋对象移交原版袋表）+ 刷新，完全不进原函数；
//   原版全满 → 扩展袋接管（equip_extension_bag_item_locked）；
//   扩展也满 → 已识别扩展源时阻断，原版源才交还原函数显示原版错误。
// 非扩展物品仍交还原函数，保持原版流程。
// 定义在匿名 namespace 之外（port 跨 TU 调用）；依赖的本 TU 匿名 namespace
// 函数（category_is_extension_backpack 等）在 TU 内可见。
VirtualBagEquipButtonResult virtual_bag_handle_backpack_button_equip_result() {
    if (!g_virtual_bag_enabled.load() || !read_world_in_world()) {
        return VirtualBagEquipButtonResult::kNotExtension;
    }
    std::unique_lock<std::mutex> lock(g_virtual_bag_mtx);
    ensure_state_loaded_locked();
    void* item = g_extension_desc_item;
    // g_extension_desc_item 只在扩展物品详情被捕获；原版背包物品详情点装备
    // 时（原版 ButtonEquipExe 函数级 hook 覆盖所有详情），用原版 UIDesc_GetData
    // 读取当前 desc 物品（原版 ButtonEquipExe b7c2c 同源）。
    if (item == nullptr && g_game != nullptr && g_inventory_extras != nullptr &&
        g_inventory_extras->read.detail_item != nullptr) {
        (void)g_inventory_extras->read.detail_item(&item);
    }
    if (item == nullptr) return VirtualBagEquipButtonResult::kNotExtension;
    int src_bag = -1;
    int src_slot = -1;
    const bool from_extension = module_slot_of_item_locked(item, &src_bag, &src_slot);
    int category = -1;
    const bool category_read = read_item_category(item, &category);
    const bool jewel = category_read && read_item_is_jewel(category);
    if (jewel) {
        return from_extension ? VirtualBagEquipButtonResult::kBlocked
                              : VirtualBagEquipButtonResult::kNotExtension;
    }
    if (!category_is_extension_backpack(category)) {
        // 扩展身份已确认后，角色装备未能完整完成时不运行原版删源路径。
        // 扩展槽装备物品 → 装备到角色（原详情按钮职责，函数 hook 化；
        // 装备按钮 proc 恒为 ButtonEquipExe，见 native 第 10 hook）。
        const ::qol::contracts::QolItemCountEncoding item_encoding = item_count_encoding(item);
        if (from_extension && item_encoding == ::qol::contracts::QolItemCountEncoding::kUnknown) {
            VIRTBAG_LOG("extension equip button blocked: category unavailable bag=%d slot=%d",
                        src_bag, src_slot);
            return VirtualBagEquipButtonResult::kBlocked;
        }
        if (from_extension && item_encoding == ::qol::contracts::QolItemCountEncoding::kNotEncoded) {
            if (!module_object_replacement_allowed_locked(src_bag, src_slot,
                                                          "character equip button")) {
                VIRTBAG_LOG("extension character equip button blocked bag=%d slot=%d",
                            src_bag, src_slot);
                return VirtualBagEquipButtonResult::kBlocked;
            }
            void* character = extension_menu_character();
            if (character != nullptr &&
                equip_module_item_on_character_locked(lock, character, item, src_bag, src_slot)) {
                extension_description_set_off();
                clear_original_desc_locked();
                finish_extension_equip_ui_locked(src_bag, nullptr);
                return VirtualBagEquipButtonResult::kHandled;
            }
            VIRTBAG_LOG("extension character equip button failed bag=%d slot=%d", src_bag,
                        src_slot);
            return VirtualBagEquipButtonResult::kBlocked;
        }
        return from_extension ? VirtualBagEquipButtonResult::kBlocked
                              : VirtualBagEquipButtonResult::kNotExtension;
    }

    int found = -1;
    for (int bag = 1; bag <= 4; ++bag) {
        int32_t bag_size = -1;
        if (g_game != nullptr && g_inventory_extras != nullptr &&
            g_inventory_extras->read.bag_size != nullptr &&
            qol::contracts::succeeded(g_inventory_extras->read.bag_size(bag, &bag_size)) &&
            bag_size == 0) {
            found = bag;
            break;
        }
    }
    if (found >= 0) {
        // 原版背包源物品 + 原版有空袋：源在原版 INVEN，直接交还原版流程装袋
        // （原版自己写袋表 + 删源 + 刷新），模块不介入。
        if (!from_extension) return VirtualBagEquipButtonResult::kNotExtension;
        if (!module_object_replacement_allowed_locked(src_bag, src_slot,
                                                      "backpack equip to original")) {
            VIRTBAG_LOG("extension backpack equip to original blocked bag=%d slot=%d",
                        src_bag, src_slot);
            return VirtualBagEquipButtonResult::kBlocked;
        }
        if (g_game == nullptr || g_write_port == nullptr ||
            g_write_port->ui.set_original_bag_item == nullptr) {
            VIRTBAG_LOG("extension backpack fn-equip bag table missing");
            return VirtualBagEquipButtonResult::kBlocked;
        }
        const ModuleUseState source_use = g_module_object_use[src_bag][src_slot];
        if (!module_object_generation_advance_locked(src_bag, src_slot,
                                                     "unequip to original")) {
            VIRTBAG_LOG("extension unequip to original blocked by active object bag=%d slot=%d",
                        src_bag, src_slot);
            return VirtualBagEquipButtonResult::kBlocked;
        }
        // 袋表接收对象成功后才允许清理扩展源及交接 ownership。
        const QolResult bag_write = g_write_port->ui.set_original_bag_item(found, item);
        if (!qol::contracts::succeeded(bag_write)) {
            g_module_object_use[src_bag][src_slot] = source_use;
            VIRTBAG_LOG_ERROR("ERROR extension backpack equip original bag write failed bag=%d src=%d/%d item=%p code=%d; source retained",
                              found, src_bag, src_slot, item, bag_write.code);
            return VirtualBagEquipButtonResult::kBlocked;
        }
        virtual_bag::Item& descriptor = g_virtual_bag_state.items[src_bag][src_slot];
        descriptor = virtual_bag::Item{};
        g_module_objects[src_bag][src_slot] = nullptr;
        g_module_object_categories[src_bag][src_slot] = 0;
        g_module_object_hashes[src_bag][src_slot] = 0;
        const uint32_t handle = g_module_object_handles[src_bag][src_slot];
        g_module_object_handles[src_bag][src_slot] = 0;
        if (handle != 0) {
            // 对象可能仍处于扩展视图借用态（控件引用），先归还再移交，
            // 否则 handover 会因 live borrow 被拒而推迟，账本与袋表状态分裂。
            if (ownership::live_state(g_ownership_ledger, handle,
                                      ownership::State::kBorrowedForView)) {
                ownership::return_from_view(&g_ownership_ledger, handle);
            }
            handover_tracked_item_locked(handle, item, "backpack fn-equip original");
        }
        extension_bag_mark_dirty_locked();
        persist_state_locked();
        extension_description_set_off();
        virtual_bag_call_original_refresh_item_area_with_guard();
        extension_refresh_bag_area();
        clear_original_desc_locked();
        if (g_module_view_installed && g_module_view_index == src_bag) {
            refresh_module_item_area_locked(src_bag);
        }
        VIRTBAG_LOG("extension backpack fn-equip original bag=%d src=%d/%d item=%p",
                    found, src_bag, src_slot, item);
        return VirtualBagEquipButtonResult::kHandled;
    }

    // 原版袋全满 → 扩展接管。equip_extension_bag_item_locked 同时支持扩展槽
    // 源与"原版 INVEN 源"（内部先扫原版库存定位物品再移除，源物品真实销毁）。
    const int target = first_empty_extension_bag_locked();
    if (target < 0) {
        return from_extension ? VirtualBagEquipButtonResult::kBlocked
                              : VirtualBagEquipButtonResult::kNotExtension;
    }
    const ExtensionBagEquipResult result = equip_extension_bag_item_locked(lock, target, item);
    if (result != ExtensionBagEquipResult::kOk) {
        QOL_LOG_WARN(QolDomain::kExtensionBag,
                     "extension backpack equip transaction failed result=%d target=%d source_extension=%d",
                         static_cast<int>(result), target, from_extension ? 1 : 0);
        return VirtualBagEquipButtonResult::kBlocked;
    }
    extension_description_set_off();
    clear_original_desc_locked();
    finish_extension_equip_ui_locked(target, nullptr);
    VIRTBAG_LOG("extension backpack fn-equip extension target=%d src=%d/%d", target,
                src_bag, src_slot);
    return VirtualBagEquipButtonResult::kHandled;
}

bool virtual_bag_handle_backpack_button_equip() {
    return virtual_bag_handle_backpack_button_equip_result() ==
           VirtualBagEquipButtonResult::kHandled;
}

// ---- Path A'2：卸袋按钮函数级接管（Stage4 第 11 hook 的扩展侧实现）----
// UIEquip_ButtonUnequipExe desc_type=1 覆盖两类袋解除：
//   · 扩展袋（mode=kModule，desc 由扩展标签二次点击打开）：走扩展解除
//     unequip_extension_bag_locked（原扩展袋卸下按钮的职责，
//     现已函数 hook 化；g_extension_desc_unequip_bag 记录袋位）。
//   · 原版袋（原版视图）：纯原版下有个边角缺陷——先 INVEN_FindSaveSlot 找
//     空槽、后清袋表[N]；FindSaveSlot 在清袋表前运行会把"即将腾空的自身
//     袋行 N"当成可用位选中，SaveItem 却在清袋表之后执行，袋对象落进容量
//     已归零的行 N → 不可见=物品消失（原版背包满时必现）。接管语义：
//       袋内容非空 → 弹"袋非空"（袋保持）；
//       原版其他袋有空位 → 放回原版（排除自身行 N）+ 清袋表[N]；
//       原版满 → 收编扩展袋空位；
//       扩展也满 → 弹"背包已满"，袋保持装备态（物品绝不消失）。
bool virtual_bag_handle_original_bag_unequip(bool* out_no_space, bool* out_not_empty) {
    if (out_no_space != nullptr) *out_no_space = false;
    if (out_not_empty != nullptr) *out_not_empty = false;
    if (!g_virtual_bag_enabled.load() || !read_world_in_world()) return false;
    std::unique_lock<std::mutex> lock(g_virtual_bag_mtx);
    ensure_state_loaded_locked();
    // 仅 desc_type=1（袋详情）卸下走接管；卸装备（desc_type=0）交原版（其
    // 收编由 CHAR_UnequipItemToInven 第 9 hook 覆盖）。
    int32_t desc_type = 0;
    if (g_game == nullptr || g_write_port->ui.desc_type == nullptr ||
        !qol::contracts::succeeded(g_write_port->ui.desc_type(&desc_type)) ||
        desc_type != 1) return false;
    // 扩展视图激活：desc 必为扩展袋（原版袋 desc 只能在原版视图触发，二次
    // 点击扩展标签打开）。识别 g_extension_desc_unequip_bag 走扩展解除。
    if (g_virtual_bag_state.mode == virtual_bag::Mode::kModule) {
        const int ext_bag = g_extension_desc_unequip_bag;
        const bool owns_desc = virtual_bag::valid_index(ext_bag) &&
                               g_virtual_bag_state.selected == ext_bag &&
                               g_virtual_bag_state.info_bag == ext_bag;
        if (owns_desc) {
            const ExtensionBagUnequipResult result = unequip_extension_bag_locked(lock, ext_bag);
            VIRTBAG_LOG("extension desc unequip bag=%d result=%d", ext_bag,
                        static_cast<int>(result));
            if (result == ExtensionBagUnequipResult::kNotEmpty && out_not_empty != nullptr) {
                *out_not_empty = true;
                return true;
            }
            if (result == ExtensionBagUnequipResult::kNoSpace && out_no_space != nullptr) {
                *out_no_space = true;
                return true;
            }
            // 已确认详情来自扩展袋时，失败也必须阻断原版 backup，避免原版卸下
            // 一个并不在物理袋表中的扩展对象。
            return true;
        }
        // 扩展态但 desc 袋记录失效：保守拦截（不交原版误卸原版袋）。
        VIRTBAG_LOG("extension desc unequip stale rejected ext_bag=%d", ext_bag);
        return true;
    }
    // 被卸袋位=当前袋：desc_type=1 恒为二次点击当前袋标签产生（原版
    // InvenBagControlEventProc N==current）。不能用 UIDesc_GetData/扫袋表
    // 反查——原版卸袋按钮回调拿的是袋标签控件 GetItem（data[0]），desc 面板
    // 数据在卸袋详情不可靠（真机实证：扩展也满时袋仍被解除+物品消失）。
    int bag_index = -1;
    if (original_bag_locked() < kNoOriginalBagSelected) {
        bag_index = original_bag_locked();
    } else {
        int32_t current_bag = -1;
        if (g_write_port->ui.current_bag != nullptr &&
            qol::contracts::succeeded(
                g_write_port->ui.current_bag(&current_bag)) &&
            current_bag < kNoOriginalBagSelected) {
            bag_index = current_bag;
        }
    }
    if (bag_index < 0 || bag_index >= 6) return false;  // 无有效袋位 → 交原版
    void* item = nullptr;
    if (g_write_port->ui.original_bag_item == nullptr ||
        !qol::contracts::succeeded(
            g_write_port->ui.original_bag_item(bag_index, &item))) return false;
    if (item == nullptr) return false;  // 该袋位无袋对象（空位详情等）→ 交原版
    int32_t empty_bag = 0;
    if (g_inventory_extras == nullptr || g_inventory_extras->read.is_empty_bag == nullptr ||
        !qol::contracts::succeeded(
            g_inventory_extras->read.is_empty_bag(bag_index, &empty_bag)) ||
        empty_bag == 0) {
        // 袋非空：模块接管弹"袋非空"（袋保持安全），不交原版。
        if (out_not_empty != nullptr) *out_not_empty = true;
        return true;
    }
    // 放回原版背包（排除自身袋行，防再次落入容量归零行）。
    int receiving_bag = -1;
    for (int bag = 0; bag < 5; ++bag) {
        if (bag == bag_index) continue;
        if (extension_save_item_on_empty(item, bag)) {
            receiving_bag = bag;
            break;
        }
    }
    if (receiving_bag >= 0) {
        (void)g_write_port->ui.set_original_bag_item(bag_index, nullptr);
        extension_bag_mark_dirty_locked();
        persist_state_locked();
        extension_description_set_off();
        virtual_bag_call_original_refresh_item_area_with_guard();
        extension_refresh_bag_area();
        clear_original_desc_locked();
        VIRTBAG_LOG("original bag unequip->original bag=%d dst=%d item=%p", bag_index,
                    receiving_bag, item);
        return true;
    }
    // 原版背包满：收编扩展袋空位（serialize 后对象移交扩展槽，袋表清空）。
    int adopt_bag = -1;
    int adopt_slot = -1;
    for (int bag = 0; bag < virtual_bag::kBagCount && adopt_bag < 0; ++bag) {
        if (g_virtual_bag_state.types[bag] == 0) continue;
        const int capacity = g_virtual_bag_state.capacities[bag];
        for (int slot = 0; slot < capacity; ++slot) {
            if (module_slot_is_assignable_locked(bag, slot)) {
                adopt_bag = bag;
                adopt_slot = slot;
                break;
            }
        }
    }
    if (adopt_bag < 0) {
        // 扩展也满：置标志由 wrapper 解锁后弹"背包已满"，袋保持装备态，
        // 物品不消失（此处绝不弹窗：锁内零 UI）。
        if (out_no_space != nullptr) *out_no_space = true;
        VIRTBAG_LOG("original bag unequip reject no space bag=%d item=%p", bag_index, item);
        return true;
    }
    std::array<uint8_t, virtual_bag::kSerializedItemBuffer> payload{};
    int payload_size = 0;
    if (!serialize_item_payload_locked(item, &payload, &payload_size)) {
        VIRTBAG_LOG("original bag unequip serialize failed bag=%d", bag_index);
        return false;  // 序列化失败极罕见：交原版，宁可原版语义也不凭空丢
    }
    virtual_bag::Item& descriptor = g_virtual_bag_state.items[adopt_bag][adopt_slot];
    descriptor.payload = payload;
    descriptor.payload_size = payload_size;
    (void)read_item_category(item, &descriptor.category);
    descriptor.count = 1;
    uint32_t adopted_handle = 0;
    if (ownership::allocate(&g_ownership_ledger, &adopted_handle) == ownership::Outcome::kOk) {
        g_module_object_handles[adopt_bag][adopt_slot] = adopted_handle;
    } else {
        g_module_object_handles[adopt_bag][adopt_slot] = 0;
        VIRTBAG_LOG("original bag unequip ledger exhausted bag=%d", bag_index);
    }
    if (!module_object_generation_advance_locked(
            adopt_bag, adopt_slot, "original bag unequip adopt")) {
        VIRTBAG_LOG("original bag unequip adopt blocked by active object bag=%d slot=%d",
                    adopt_bag, adopt_slot);
        return false;
    }
    if (!extension_bag_rebind_item_handle(item)) {
        descriptor = virtual_bag::Item{};
        if (adopted_handle != 0) ownership::release(&g_ownership_ledger, adopted_handle);
        g_module_object_handles[adopt_bag][adopt_slot] = 0;
        VIRTBAG_LOG("original bag unequip handle registration failed bag=%d slot=%d",
                    adopt_bag, adopt_slot);
        return false;
    }
    g_module_objects[adopt_bag][adopt_slot] = item;
    g_module_object_categories[adopt_bag][adopt_slot] = descriptor.category;
    g_module_object_hashes[adopt_bag][adopt_slot] = virtual_bag::payload_hash(descriptor);
    (void)g_write_port->ui.set_original_bag_item(bag_index, nullptr);
    extension_bag_mark_dirty_locked();
    persist_state_locked();
    extension_description_set_off();
    clear_original_desc_locked();
    if (g_module_view_installed && g_module_view_index == adopt_bag) {
        refresh_module_item_area_locked(adopt_bag);
    } else {
        virtual_bag_call_original_refresh_item_area_with_guard();
    }
    extension_refresh_bag_area();
    VIRTBAG_LOG("original bag unequip->extension bag=%d dst=%d/%d item=%p", bag_index,
                adopt_bag, adopt_slot, item);
    return true;
}

#include "plugins/extension_bag/extension_bag_api_impl.inc"

// P3c：extension_bag_save_game_handler 与 8 条存档 callsite claims 已删除——SAVE_Save
// 入口由 core.save 的 hook:SAVE_Save 接管（联合保存经协调器），EB 数据面走 save.slot_data。

// D2c-2b-2：move_merge / source protection wrapper（X1 handler；须在 claims 之前定义）。
#include "plugins/extension_bag/move_merge.inc"

// D2a-3b-1：8 条声明式资源（须在全部 wrapper/全局定义之后）。
#include "plugins/extension_bag/extension_bag_claims.inc"
