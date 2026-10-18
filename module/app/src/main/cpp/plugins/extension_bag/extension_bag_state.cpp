#include "plugins/extension_bag/extension_bag_internal.h"

#include "plugins/extension_bag/extension_bag_context.h"
#include "plugins/extension_bag/extension_bag_geometry.h"

// 共享锁与 dirty 标记在文件作用域（非匿名 namespace）：extension_bag_persistence.cpp
// 经 extern 引用同一实例（save.slot_data 回调/排空面与 UI 面共享锁边界，rulebook §2）。
std::mutex g_virtual_bag_mtx;
bool g_item_state_dirty = false;
std::atomic<uint64_t> g_item_state_revision{0};
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
std::array<std::array<void*, virtual_bag::kSlotCount>, virtual_bag::kBagCount>
    g_module_objects{};
std::array<std::array<ModuleUseState, virtual_bag::kSlotCount>, virtual_bag::kBagCount>
    g_module_object_use{};
thread_local ModuleUseToken* g_active_module_use_token = nullptr;
std::array<std::array<int, virtual_bag::kSlotCount>, virtual_bag::kBagCount>
    g_module_object_categories{};
std::array<std::array<uint32_t, virtual_bag::kSlotCount>, virtual_bag::kBagCount>
    g_module_object_hashes{};
uint32_t g_module_object_handles[virtual_bag::kBagCount][virtual_bag::kSlotCount]{};
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
std::array<uint64_t, 11> g_extension_desc_item_ids{};
std::array<bool, 11> g_extension_desc_item_bound{};
void* g_extension_desc_item = nullptr;
int g_extension_desc_item_bag = -1;
int g_extension_desc_item_slot = -1;
ExtensionDestroyOkFn g_extension_destroy_original_ok = nullptr;
ExtensionDestroyOkFn g_extension_sell_original_ok = nullptr;
ExtensionDestroyOkFn g_extension_destroy_original_cancel = nullptr;
ExtensionDestroyOkFn g_extension_sell_original_cancel = nullptr;
bool g_extension_sell_apply_variant_discount = true;
ExtensionDrag g_extension_drag{};
virtual_bag::ExtensionDragSession g_extension_drag_session{};
std::atomic<uint64_t> g_p5_observation_sequence{0};
std::atomic<uint64_t> g_p5_last_item_query_sample_ms{0};
std::atomic<uint64_t> g_p5_last_panel_move_sample_ms{0};
thread_local bool g_p5_observation_active = false;
thread_local bool g_refresh_restore_suppress = false;
thread_local bool g_in_native_call = false;
void* g_tab_bag_items[virtual_bag::kBagCount] = {};

void extension_bag_mark_dirty_locked() {
    g_item_state_dirty = true;
    g_item_state_revision.fetch_add(1, std::memory_order_release);
}

uint64_t extension_bag_item_revision_locked() {
    return g_item_state_revision.load(std::memory_order_acquire);
}

const qol::contracts::QolExtensionBagWritePort* extension_bag_write_api() { return g_write_port; }

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

void* extension_create_item(int category, int arg1, int arg2, int arg3) {
    const auto* api = extension_bag_write_api();
    if (api == nullptr || api->inventory.create_item == nullptr) return nullptr;
    void* item = nullptr;
    return qol::contracts::succeeded(
               api->inventory.create_item(category, arg1, arg2, arg3, &item))
               ? item
               : nullptr;
}

const qol::contracts::QolItemCountApi* item_count_api() {
    return g_game == nullptr ? nullptr : &g_game->item_count;
}

bool mark_native_bag_object(void* item) {
    const auto* api = item_count_api();
    if (api == nullptr || api->read_encoded_field == nullptr ||
        api->write_encoded_field == nullptr || item == nullptr) return false;
    uint32_t field = 0;
    if (!qol::contracts::succeeded(api->read_encoded_field(item, &field))) return false;
    field = qol::features::extension_bag::write_native_bag_object_marker(field);
    return qol::contracts::succeeded(api->write_encoded_field(item, field));
}

// G-8/H4 共享扩展身份判据：所有事件层与函数层守卫都反查同一份
// g_module_objects，避免 source 判定在不同防线间漂移。调用方必须已持有
// g_virtual_bag_mtx。
bool module_slot_of_item_locked(void* item, int* out_bag, int* out_slot) {
    if (item == nullptr || out_bag == nullptr || out_slot == nullptr) return false;
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

// GetChild 结果必须过双重校验：面板重建窗口里 GetChild(slot≥count) 返回不可读的
// 垃圾指针（tombstone_02），先 GetCount 判界消除越界源，再 GetUserType==2 验类型。
// root 本身也可能被面板重建替换——调用方应实时通过 UI adapter 查询，
// 不要使用 install 时的快照。
void* valid_child_locked(void* root, int slot) {
    return extension_bag_valid_child(root, slot);
}
