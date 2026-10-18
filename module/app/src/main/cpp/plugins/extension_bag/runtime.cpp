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
#include "plugins/extension_bag/extension_bag_internal.h"

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

#define current_env current_env_internal
#define isolation_now_ms_locked isolation_now_ms_locked_internal
#define extension_apply_stuff extension_apply_stuff_internal
#define read_item_cumulate_count read_item_cumulate_count_internal
#define read_save_is_ok read_save_is_ok_internal
#define read_has_empty_mercenary_slot read_has_empty_mercenary_slot_internal
#define extension_description_set_off extension_description_set_off_internal
#define extension_use_item extension_use_item_internal
#define extension_make_mercenary extension_make_mercenary_internal
#define extension_refresh_bag_area extension_refresh_bag_area_internal
#define item_count_encoding item_count_encoding_internal
#define original_count_view original_count_view_internal
#define category_is_equip category_is_equip_internal
#define category_is_extension_backpack category_is_extension_backpack_internal
#define handle_extension_tab_click_locked handle_extension_tab_click_locked_internal

bool apply_extension_material_to_slot_locked(std::unique_lock<std::mutex>& lock,
                                             int src_bag, int src_slot, int dst_bag,
                                             int dst_slot, void* material, void* target_item);

namespace {

constexpr size_t kPopupStateSize = 0x40;
constexpr int kPopupStateCount = 27;
constexpr int64_t kCellStepY = 0x46;
constexpr size_t kInventorySlotStride = 16;
// Extension tab ControlObject rects are relative to the item-list root, whose
// absolute origin is (kGridX, kGridY). Their visuals keep the existing overlay
// positions so this phase changes input ownership without moving the UI.
constexpr int64_t kExtensionTabX = kCellX - kGridX;
constexpr int64_t kExtensionTabY = kCellY - kGridY;
constexpr int64_t kExtensionTabWidth = 0x70;
constexpr int64_t kExtensionTabHeight = 0x30;

uint64_t isolation_now_ms_locked();

}  // namespace

bool extension_save_item_on_empty(void* item, int bag) {
    const auto* api = extension_bag_write_api();
    if (api == nullptr || api->inventory.save_item_on_empty == nullptr) return false;
    int32_t result = 0;
    return qol::contracts::succeeded(api->inventory.save_item_on_empty(item, bag, &result)) &&
           result != 0;
}

namespace {

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

}  // namespace

bool extension_free_item(void* item) {
    const auto* api = extension_bag_write_api();
    return api != nullptr && api->inventory.free_item != nullptr &&
           qol::contracts::succeeded(api->inventory.free_item(item));
}

namespace {

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

}  // namespace

bool read_item_is_jewel(int category) {
    if (g_item_facts == nullptr || g_item_facts->is_jewel_category == nullptr) return false;
    int32_t is_jewel = 0;
    return qol::contracts::succeeded(
               g_item_facts->is_jewel_category(category, &is_jewel)) &&
           is_jewel != 0;
}

namespace {

bool read_item_category_predicate(int category,
                                  QolResult (*predicate)(int32_t, int32_t*),
                                  bool* out_value) {
    if (g_game == nullptr || predicate == nullptr || out_value == nullptr) return false;
    int32_t value = 0;
    if (!qol::contracts::succeeded(predicate(category, &value))) return false;
    *out_value = value != 0;
    return true;
}

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

namespace {

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

#define clear_original_desc_locked clear_original_desc_locked_internal
#define original_bag_locked original_bag_locked_internal
#define set_original_bag_locked set_original_bag_locked_internal
#define bag_size_word_locked bag_size_word_locked_internal
#define set_bag_size_word_locked set_bag_size_word_locked_internal
#define original_bag_button_index original_bag_button_index_internal
#define install_module_view_locked install_module_view_locked_internal
#define ensure_module_view_root_current_locked ensure_module_view_root_current_locked_internal
#define restore_module_view_locked restore_module_view_locked_internal
#define refresh_projection_if_overwritten_locked refresh_projection_if_overwritten_locked_internal
#define refresh_projected_module_view_after_move_locked refresh_projected_module_view_after_move_locked_internal
#define refresh_module_item_area_locked refresh_module_item_area_locked_internal
#define sync_projected_slot_locked sync_projected_slot_locked_internal
#define module_item_locked module_item_locked_internal
#define prepare_main_menu_locked prepare_main_menu_locked_internal
#define ensure_state_loaded_locked ensure_state_loaded_locked_internal
#define persist_state_locked persist_state_locked_internal
#define log_exit_trace_locked log_exit_trace_locked_internal
#define delete_extension_tab_buttons_locked delete_extension_tab_buttons_locked_internal
#define disable_extension_tab_buttons_locked disable_extension_tab_buttons_locked_internal
#define install_extension_tab_buttons_locked install_extension_tab_buttons_locked_internal
#define extension_tab_buttons_complete_locked extension_tab_buttons_complete_locked_internal
#define free_module_object_locked free_module_object_locked_internal
#define materialize_module_item_locked materialize_module_item_locked_internal
#define release_temporary_item_locked release_temporary_item_locked_internal
#define inventory_slot_locked inventory_slot_locked_internal
#define item_pointer_known_locked item_pointer_known_locked_internal
#define teardown_extension_bag_hosts_locked teardown_extension_bag_hosts_locked_internal
#define module_cache_clear_preflight_locked module_cache_clear_preflight_locked_internal
#define clear_module_cache_locked clear_module_cache_locked_internal
#define commit_pending_extension_tab_locked commit_pending_extension_tab_locked_internal
#define reset_extension_desc_unequip_hook_locked reset_extension_desc_unequip_hook_locked_internal
#define reset_extension_desc_item_hooks_locked reset_extension_desc_item_hooks_locked_internal
#define restore_extension_desc_popup_callbacks_locked restore_extension_desc_popup_callbacks_locked_internal
#define move_original_to_extension_locked move_original_to_extension_locked_internal
#define move_original_to_extension_slot_locked move_original_to_extension_slot_locked_internal
#define move_extension_to_original_locked move_extension_to_original_locked_internal
#define move_extension_to_extension_locked move_extension_to_extension_locked_internal
#define route_projected_session_to_tab_locked route_projected_session_to_tab_locked_internal
#define module_slot_is_assignable_locked module_slot_is_assignable_locked_internal
#define module_object_replacement_allowed_locked module_object_replacement_allowed_locked_internal
#define extension_tab_hit extension_tab_hit_internal
#define serialize_item_payload_locked serialize_item_payload_locked_internal
#define load_item_payload_locked load_item_payload_locked_internal
#define generate_transaction_id_locked generate_transaction_id_locked_internal
#define txn_record_pending_locked txn_record_pending_locked_internal
#define txn_abort_locked txn_abort_locked_internal
#define isolate_pending_locked isolate_pending_locked_internal
#define isolate_ext2orig_failure_locked isolate_ext2orig_failure_locked_internal
#define original_inventory_contains_payload_locked original_inventory_contains_payload_locked_internal
#define original_inventory_payload_slot_locked original_inventory_payload_slot_locked_internal
#define original_inventory_handle_slot_locked original_inventory_handle_slot_locked_internal
#define recover_pending_transaction_locked recover_pending_transaction_locked_internal
#define try_equip_on_extension_tab_drop_locked try_equip_on_extension_tab_drop_locked_internal
#define extension_source_should_equip_locked extension_source_should_equip_locked_internal
#define equip_extension_source_on_tab_locked equip_extension_source_on_tab_locked_internal
#define remove_item_direct_unlocked remove_item_direct_unlocked_internal
#define make_desc_equip_gate make_desc_equip_gate_runtime_internal
#define consume_extension_item_after_native_locked consume_extension_item_after_native_locked_runtime_internal

bool route_projected_session_to_tab_locked(std::unique_lock<std::mutex>& lock, int target_bag);

// 扩展标签控件事件处理：TouchHandle 转发的控件事件（0x02=click 松开确认，
// 0x04=drop 到袋）。挂袋容器后与原版袋标签同链：松开 → TouchHandle 判定。
void queue_extension_tab_click_locked(int extension_bag);
bool try_equip_on_extension_tab_drop_locked(std::unique_lock<std::mutex>& lock,
                                            int index, void* moving_control);
bool extension_source_should_equip_locked(int target_bag);
bool equip_extension_source_on_tab_locked(std::unique_lock<std::mutex>& lock, int target_bag,
                                          int source_bag, int source_slot);
bool move_original_to_extension_locked(std::unique_lock<std::mutex>& lock, int dst_bag,
                                       void* moving_control);
bool move_original_to_extension_slot_locked(
    std::unique_lock<std::mutex>& lock, int src_bag, int src_slot, int dst_bag,
    qol::contracts::QolItemHandle expected_item);
bool move_extension_to_original_locked(std::unique_lock<std::mutex>& lock, int src_bag,
                                       int src_slot, int target_bag);
bool move_extension_to_extension_locked(int src_bag, int src_slot, int dst_bag,
                                        int requested_dst_slot);
bool persist_state_locked(bool force);
void refresh_projected_module_view_after_move_locked();
void refresh_projection_if_overwritten_locked();
bool sync_projected_slot_locked(int bag, int slot);
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
bool is_backpack_category(int category) {
    // 这是 State::types 使用的 BagType 判定：1..4 派生容量 4/8/12/16，
    // 只适用于已归一化的扩展袋类型。它不是原生物品类别判定；原生物品
    // 详情必须使用 category_is_extension_backpack 的 ITEMCLASSBASE +2 规则。
    return category >= 1 && category <= 4;
}

bool extension_tab_control_is_current_locked(int index, void* control);
bool extension_tab_buttons_complete_locked();
bool extension_tab_buttons_have_controls_locked();
void restore_module_view_locked();

uint64_t extension_tab_item_proc(void* ctrl, uint64_t event, void* x2, void* param) {
    const uint64_t observation_token = virtual_bag_observe_item_proc_pre(ctrl, event, x2, param);
    const auto finish = [observation_token, ctrl, event, x2, param](uint64_t result) {
        virtual_bag_observe_item_proc_post(observation_token, ctrl, event, x2, param, result);
        return result;
    };
    if (event == 0x02) {
        uint64_t result = 1;
        {
            std::unique_lock<std::mutex> lock(g_virtual_bag_mtx);
            if (g_virtual_bag_enabled.load() && extension_bag_session_admitted_locked()) {
                const int index = extension_tab_index(ctrl);
                if (index >= 0) queue_extension_tab_click_locked(index);
            }
        }
        // Observation post reacquires this mutex, so finish only after leaving the scope.
        return finish(result);  // 松开确认已处理（原版袋标签点击切袋同样返回 1，b8c60）
    }
    // 0x04 = 拖动 drop 到袋控件。无论原版源还是扩展源，只要目标袋未装备且
    // 源类别为背包物品（category 1..4）就走装备；其他情况走移动。装备路径
    // 返回 0，让 TouchHandle 按原版语义复位 moving。
    if (event == 0x04) {
        uint64_t result = 0;
        {
            std::unique_lock<std::mutex> lock(g_virtual_bag_mtx);
            if (!g_virtual_bag_enabled.load() || !extension_bag_session_admitted_locked()) {
                result = 1;
            } else {
            const int index = extension_tab_index(ctrl);
            if (index >= 0 && extension_tab_control_is_current_locked(index, ctrl) &&
                g_extension_drag_session.phase != virtual_bag::DragPhase::kIdle) {
                const int logical_bag = virtual_bag::extension_logical_bag(index);
                const bool handled = logical_bag >= 0 &&
                                     route_projected_session_to_tab_locked(lock, logical_bag);
                if (logical_bag < 0) {
                    VIRTBAG_LOG("cross tab reject reason=target_tab_index_invalid index=%d", index);
                }
                result = handled ? 1 : 0;
            } else {
                void* moving = touch_moving_item_control_locked();
                // 原版不可堆叠装备在 drop 事件到达时可能已经清掉
                // ITEM_CTRL_MOVING_FLAG，但事件参数仍保留真实 source control。
                // 优先使用 TouchHandle 状态，只有状态已清空时才回退到事件源，避免
                // 放宽普通点击/无效控件的处理范围。
                if (moving == nullptr && param != nullptr) {
                    moving = *reinterpret_cast<void**>(reinterpret_cast<uint8_t*>(param) + 8);
                }
                if (index >= 0 && moving != nullptr) {
                    void* data = extension_bag_ui_data(moving);
                    void* item = data != nullptr ? *reinterpret_cast<void**>(data) : nullptr;
                    int module_bag = -1;
                    int module_slot = -1;
                    const bool extension_source =
                        item != nullptr && module_slot_of_item_locked(item, &module_bag, &module_slot);
                    const int category = extension_source
                        ? g_virtual_bag_state.items[module_bag][module_slot].category
                        : original_item_category_locked(item);
                    const bool backpack_item = is_backpack_category(category);
                    if (g_virtual_bag_state.types[index] == 0 && backpack_item) {
                        // 背包类物品进入空扩展标签 = 装备为该袋的袋对象；来源可以是
                        // 原版背包，也可以是另一个扩展袋。两者都由同一事务处理。
                        try_equip_on_extension_tab_drop_locked(lock, index, moving);
                    } else if (extension_source) {
                        // 目标袋已有背包对象，或来源是装备/其他物品：一律按普通
                        // 扩展袋间移动处理，不能把“背包槽装备”误当成人物装备。
                        const bool moved = move_extension_to_extension_locked(
                            module_bag, module_slot, index, -1);
                        if (moved) {
                            persist_state_locked(false);
                            refresh_projected_module_view_after_move_locked();
                        }
                        VIRTBAG_LOG("extension tab drop extension move result=%d index=%d category=%d",
                                    moved ? 1 : 0, index, category);
                    } else {
                        const bool moved = move_original_to_extension_locked(lock, index, moving);
                        VIRTBAG_LOG("extension tab drop original move result=%d index=%d category=%d equipped=%d",
                                    moved ? 1 : 0, index, category,
                                    g_virtual_bag_state.types[index] != 0 ? 1 : 0);
                    }
                }
            }
            }
        }
        return finish(result);
    }
    // 其余事件（含 0x10 拖动发起查询：TouchHandle_MoveOn @a2fec-a304c 派发，
    // proc 返回 1 即登记 moving 控件）一律返回 0——兜底返回 1 会让标签成为
    // 拖动源（标签可被拖出背包物品的真因，且拖出物可被 drop 入库造成复制）。
    // 原版控件对未处理事件同样返回 0。
    return finish(0);
}

// 扩展标签 = ControlItem（与原版袋标签同类）：挂袋容器（0x3049e0+0x50），
// SetUserType(2) + SetControlProc（自定义，0x02 松开确认切换/详情 toggle）。
// data[0] = 按袋 types 物化的真实背包物品对象（TouchHandle/详情链合法）。
// 手工父链累加（自上而下）：计算控件绝对位置。CO_RECT 为 i64 相对父值。
void delete_extension_tab_buttons_locked() {
    for (void*& button : g_extension_tab_buttons) {
        if (button == nullptr) continue;
        if (extension_bag_ui_delete_control(button)) {
            button = nullptr;
        } else {
            VIRTBAG_LOG("extension tab delete failed control=%p", button);
        }
    }
}

void invalidate_extension_tab_buttons_locked() {
    cancel_projected_drag_session_locked("extension-tab-root-change");
    ++g_inventory_generation;
    g_pending_extension_tab = -1;
    g_pending_extension_tab_generation = 0;
    g_extension_tab_buttons.fill(nullptr);
    g_extension_tab_root = nullptr;
    g_extension_tab_generation = 0;
}

void install_extension_tab_buttons_locked() {
    // 配置门控：扩展背包关闭时不得挂载页签（3 个宿主之一）。启用/禁用均以
    // g_virtual_bag_enabled 为准；调用方不再各自判断。
    if (!g_virtual_bag_enabled.load() || !extension_bag_session_admitted_locked()) return;
    void* bag_container = extension_bag_ui_root(qol::contracts::QolUiRootKind::kEquipBagContainer);
    if (bag_container == nullptr) return;
    // 容器就位守卫：面板打开初期 0x50 槽位指向占位容器（无原版袋标签子控件，
    // rect 未定位）。此时挂载会让标签落在占位容器上（绝对 x≈296 出位）。
    // 等原版 6 袋标签就位（child count ≥ 6）后再挂载；未就位直接跳过，
    // 由 draw 每帧检测容器就位后重试。
    int bag_child_count = 0;
    if (!extension_bag_ui_child_count(bag_container, &bag_child_count) || bag_child_count < 6) return;
    // 动态计算：打开面板后，原版袋 0 按钮的绝对位置 =
    // 袋容器绝对（父链累加，袋 0 相对容器为 (0,0)）。缓存至面板重建。
    // 挂载层 = 袋容器（与原版 6 袋标签同父，跟随容器动画，天然不漂移）。
    int64_t bag0_abs_x = 0, bag0_abs_y = 0;
    extension_bag_control_abs_pos(bag_container, &bag0_abs_x, &bag0_abs_y);
    void* mount_parent = bag_container;
    int64_t mount_abs_x = 0, mount_abs_y = 0;
    extension_bag_control_abs_pos(mount_parent, &mount_abs_x, &mount_abs_y);
    // 标签相对挂载层 = 原版袋列右侧（袋 0 绝对 + (68,2)，减挂载层绝对）
    const int64_t tab_rel_x = bag0_abs_x + 68 - mount_abs_x;
    const int64_t tab_rel_y = bag0_abs_y + 2 - mount_abs_y;
    if (g_extension_tab_generation == g_inventory_generation &&
        g_extension_tab_root == mount_parent && extension_tab_buttons_complete_locked()) {
        return;
    }
    // A replaced root owns the old controls; invalidate them without dereferencing stale handles.
    if (g_extension_tab_root != nullptr && g_extension_tab_root != mount_parent) {
        invalidate_extension_tab_buttons_locked();
    } else if (g_extension_tab_root == mount_parent || extension_tab_buttons_have_controls_locked()) {
        delete_extension_tab_buttons_locked();
        if (extension_tab_buttons_have_controls_locked()) {
            VIRTBAG_LOG("extension tab install aborted reason=previous_controls_not_deleted root=%p",
                        mount_parent);
            return;
        }
        g_extension_tab_root = nullptr;
        g_extension_tab_generation = 0;
    }
    refresh_tab_bag_items_locked();
    std::array<void*, virtual_bag::kBagCount> created{};
    auto discard_created = [&]() {
        bool all_deleted = true;
        for (int index = 0; index < virtual_bag::kBagCount; ++index) {
            void*& control = created[static_cast<size_t>(index)];
            if (control == nullptr) continue;
            if (!extension_bag_ui_delete_control(control)) {
                all_deleted = false;
                g_extension_tab_buttons[static_cast<size_t>(index)] = control;
                VIRTBAG_LOG("extension tab cleanup failed index=%d control=%p", index, control);
            }
            control = nullptr;
        }
        if (!all_deleted) {
            // Keep the failed live controls tracked so a same-root retry can delete them.
            g_extension_tab_root = mount_parent;
            g_extension_tab_generation = 0;
        }
        return all_deleted;
    };
    bool complete = true;
    for (int index = 0; index < virtual_bag::kBagCount; ++index) {
        // ControlItem_Create 语义组装：Add(parent, 0, 0, type=3, proc) + SetControlProc + SetUserType(2)。
        // 第 5 参 = proc，经 CreateControlInfo 写入 +0x90（CO_PROC，TouchHandle 统一事件分发）。
        // 传 nullptr 会让 +0x90 为空 → TouchHandle 命中后无法分发到 +0x98 control proc（点击失效）。
        void* ctrl = nullptr;
        if (!extension_bag_ui_create_control(mount_parent, &ctrl) ||
            !extension_bag_ui_set_control_proc(ctrl, &extension_tab_item_proc) ||
            !extension_bag_ui_set_control_user_type(ctrl, 2) ||
            !extension_bag_ui_unuse_control_event_move(ctrl)) {
            if (ctrl != nullptr && !extension_bag_ui_delete_control(ctrl)) {
                VIRTBAG_LOG("extension tab cleanup failed index=%d control=%p", index, ctrl);
                created[static_cast<size_t>(index)] = ctrl;
                complete = false;
                break;
            }
            ctrl = nullptr;
        }
        if (ctrl == nullptr) {
            VIRTBAG_LOG("extension tab create failed index=%d", index);
            complete = false;
            break;
        }
        created[static_cast<size_t>(index)] = ctrl;
        // rect：与原版袋标签同尺寸 57x57（loc6/loc9 底框 47x47 居中填充成背景框）。
        // 相对场景根（不随切袋移动），由动态计算的原版袋列绝对位置换算。
        const int64_t row_y = tab_rel_y + index * 70;
        if (!extension_bag_ui_set_rect(ctrl, {tab_rel_x, row_y, 57, 57})) {
            VIRTBAG_LOG("extension tab setup failed reason=set_rect index=%d control=%p", index, ctrl);
            complete = false;
            break;
        }
        // data：16B（+0x0=item、+0x8..0xb=标志），装箱背包物品对象
        void* data = nullptr;
        if (!extension_bag_ui_allocate_control_data(0x10, &data) || data == nullptr) {
            VIRTBAG_LOG("extension tab setup failed reason=allocate_control_data index=%d control=%p",
                        index, ctrl);
            complete = false;
            break;
        }
        std::memset(data, 0, 0x10);
        *reinterpret_cast<void**>(data) = g_tab_bag_items[index];
        if (!extension_bag_ui_set_control_data(ctrl, data)) {
            VIRTBAG_LOG("extension tab setup failed reason=set_control_data index=%d control=%p",
                        index, ctrl);
            complete = false;
            break;
        }
    }
    if (extension_bag_ui_root(qol::contracts::QolUiRootKind::kEquipBagContainer) != mount_parent) {
        // The game replaced this tree while controls were being created; its handles are stale.
        invalidate_extension_tab_buttons_locked();
        VIRTBAG_LOG("extension tab install aborted reason=root_replaced during_create root=%p",
                    mount_parent);
        return;
    }
    if (!complete) {
        (void)discard_created();
        return;
    }
    g_extension_tab_buttons = created;
    g_extension_tab_root = mount_parent;
    g_extension_tab_generation = g_inventory_generation;
    VIRTBAG_LOG("extension tabs installed generation=%llu root=%p",
                static_cast<unsigned long long>(g_inventory_generation), bag_container);
}

void disable_extension_tab_buttons_locked() {
    void* live_root = extension_bag_ui_root(qol::contracts::QolUiRootKind::kEquipBagContainer);
    if (g_extension_tab_root != nullptr && live_root == g_extension_tab_root) {
        delete_extension_tab_buttons_locked();
    } else if (g_extension_tab_root != nullptr) {
        VIRTBAG_LOG("extension tab references invalidated reason=root_not_live saved=%p live=%p",
                    g_extension_tab_root, live_root);
    }
    // The game owns controls; stale-root references are invalidated, never deleted.
    invalidate_extension_tab_buttons_locked();
}

void teardown_extension_bag_hosts_locked() {
    // Disable all three UI hosts in the same lifecycle transaction.
    store_teardown_locked();
    mix_teardown_locked();
    restore_module_view_locked();
    disable_extension_tab_buttons_locked();
}

int raw_direct_bag_locked() {
    if (g_game == nullptr || g_write_port->ui.current_bag == nullptr) return -1;
    int32_t bag = -1;
    return qol::contracts::succeeded(g_write_port->ui.current_bag(&bag)) ? bag : -1;
}

int raw_got_bag_locked() {
    return raw_direct_bag_locked();
}

int raw_desc_type_locked() {
    if (g_game == nullptr || g_write_port->ui.desc_type == nullptr) return -1;
    int32_t type = -1;
    return qol::contracts::succeeded(g_write_port->ui.desc_type(&type)) ? type : -1;
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

JNIEnv* current_env() {
    JavaVM* jvm = g_jvm();
    if (jvm == nullptr) return nullptr;
    JNIEnv* env = nullptr;
    if (jvm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6) == JNI_OK) return env;
    return jvm->AttachCurrentThread(&env, nullptr) == JNI_OK ? env : nullptr;
}

void ensure_state_loaded_locked();
bool persist_state_locked(bool force = false);
void clear_original_desc_locked();
bool extension_tab_hit(int index, int64_t x, int64_t y);
int original_bag_locked();
void set_original_bag_locked(int bag);
bool bag_size_word_locked(int bag, uint32_t* out_word);
bool set_bag_size_word_locked(int bag, uint32_t word);
int original_bag_button_index(int64_t x, int64_t y);
bool install_module_view_locked(int bag);
bool ensure_module_view_root_current_locked();
void restore_module_view_locked();
void refresh_projection_if_overwritten_locked();
void refresh_projected_module_view_after_move_locked();
void refresh_module_item_area_locked(int index);
void* module_item_locked(int bag, int slot);
void recover_pending_transaction_locked(std::unique_lock<std::mutex>& lock);
bool prepare_main_menu_locked();
void teardown_extension_bag_hosts_locked();
bool free_module_object_locked(int bag, int slot, void* use_token = nullptr);
bool module_slot_is_assignable_locked(int bag, int slot);
bool module_object_replacement_allowed_locked(int bag, int slot, const char* context);
bool move_original_to_extension_locked(std::unique_lock<std::mutex>& lock, int dst_bag,
                                       void* moving_control);
bool move_original_to_extension_slot_locked(std::unique_lock<std::mutex>& lock, int src_bag,
                                             int src_slot, int dst_bag,
                                             qol::contracts::QolItemHandle expected_item =
                                                 qol::contracts::kInvalidItemHandle);
bool move_extension_to_original_locked(std::unique_lock<std::mutex>& lock, int src_bag,
                                       int src_slot, int target_bag);
bool move_extension_to_extension_locked(int src_bag, int src_slot, int dst_bag,
                                        int requested_dst_slot = -1);
bool handle_bag_drop_release_locked(std::unique_lock<std::mutex>& lock, int64_t x, int64_t y);
bool item_pointer_known_locked(void* item);
void make_desc_equip_gate(void* ctrl, void* arg);
void extension_desc_item_execute_at(size_t index, void* button);
void reset_extension_desc_item_hooks_locked();
void install_extension_desc_item_hooks_locked(void* item);
void restore_extension_desc_popup_callbacks_locked();
bool consume_extension_item_after_native_locked(int bag, int slot, void* item,
                                                 int before_count, int observed_before,
                                                 const char* context, void* use_token);
bool extension_tab_control_is_current_locked(int index, void* control);
void handle_extension_tab_click_impl_locked(int extension_bag,
                                            std::unique_lock<std::mutex>* unlockable_lock);

bool extension_tab_buttons_complete_locked() {
    for (void* button : g_extension_tab_buttons) {
        if (button == nullptr) return false;
    }
    return true;
}

bool extension_tab_buttons_have_controls_locked() {
    for (void* button : g_extension_tab_buttons) {
        if (button != nullptr) return true;
    }
    return false;
}

void reset_extension_desc_unequip_hook_locked() {
    // 卸下按钮由函数级 ButtonUnequipExe hook 承担，
    // 仅清 desc 袋记录。
    g_extension_desc_unequip_bag = -1;
}

void install_extension_desc_unequip_hook_locked(int extension_bag) {
    reset_extension_desc_unequip_hook_locked();
    if (!virtual_bag::valid_index(extension_bag)) return;
    // 扩展袋 desc 打开时记录袋位，供函数级 ButtonUnequipExe hook
    // （virtual_bag_handle_original_bag_unequip）识别扩展袋解除路径。
    g_extension_desc_unequip_bag = extension_bag;
    VIRTBAG_LOG("extension desc unequip armed bag=%d", extension_bag);
}

// P3：扩展袋切换音效，与原版袋按钮同款（UIEquip_InvenBagControlEventProc b8c34：Play(0x11)）。
void play_extension_switch_sound() {
    if (g_game == nullptr || g_write_port->ui.play_switch_sound == nullptr) return;
    (void)g_write_port->ui.play_switch_sound();
}

void handle_extension_tab_click_locked(int extension_bag) {
    handle_extension_tab_click_impl_locked(extension_bag, nullptr);
}

void handle_pending_extension_tab_click_locked(std::unique_lock<std::mutex>& lock,
                                               int extension_bag) {
    handle_extension_tab_click_impl_locked(extension_bag, &lock);
}

void handle_extension_tab_click_impl_locked(int extension_bag,
                                            std::unique_lock<std::mutex>* unlockable_lock) {
    if (!virtual_bag::valid_index(extension_bag) ||
        !extension_bag_session_admitted_locked()) return;
    if (g_virtual_bag_state.mode == virtual_bag::Mode::kOriginal) {
        const int original_bag = original_bag_locked();
        if (original_bag == virtual_bag::kOriginalTaskBag) {
            constexpr uint32_t kCapacityMask = (1u << 25) - 1u;
            void* panel_root = extension_bag_ui_root(qol::contracts::QolUiRootKind::kEquipItemRoot);
            const int target_capacity = g_virtual_bag_state.capacities[extension_bag];
            const bool target_ready = target_capacity > 0 && panel_root != nullptr;
            int projection_host = -1;
            if (target_ready) {
                for (int candidate = 0;
                     candidate < virtual_bag::kOriginalTransactionBagCount; ++candidate) {
                    uint32_t size_word = 0;
                    if (!bag_size_word_locked(candidate, &size_word) ||
                        (size_word & kCapacityMask) == 0) continue;
                    bool controls_ready = true;
                    for (int slot = 0; slot < target_capacity; ++slot) {
                        if (valid_child_locked(panel_root, slot) == nullptr) {
                            controls_ready = false;
                            break;
                        }
                    }
                    if (controls_ready) {
                        projection_host = candidate;
                        break;
                    }
                }
            }
            if (projection_host < 0) {
                VIRTBAG_LOG(
                    "extension tab reject reason=projection_host_unavailable source_original_bag=%d target=%d",
                    original_bag, extension_bag);
                return;
            }
            set_original_bag_locked(projection_host);
            g_original_current_direct = static_cast<uint8_t>(projection_host);
            g_original_current_got = static_cast<uint8_t>(projection_host);
            if (!install_module_view_locked(extension_bag)) {
                set_original_bag_locked(original_bag);
                g_original_current_direct = static_cast<uint8_t>(original_bag);
                g_original_current_got = static_cast<uint8_t>(original_bag);
                VIRTBAG_LOG(
                    "extension tab reject reason=projection_host_unavailable source_original_bag=%d target=%d host=%d",
                    original_bag, extension_bag, projection_host);
                return;
            }
            if (virtual_bag::click(&g_virtual_bag_state, extension_bag) !=
                virtual_bag::ClickResult::kSelected) {
                restore_module_view_locked();
                set_original_bag_locked(original_bag);
                g_original_current_direct = static_cast<uint8_t>(original_bag);
                g_original_current_got = static_cast<uint8_t>(original_bag);
                VIRTBAG_LOG(
                    "extension tab reject reason=projection_host_unavailable source_original_bag=%d target=%d host=%d",
                    original_bag, extension_bag, projection_host);
                return;
            }
            persist_state_locked();
            VIRTBAG_LOG("extension tab selected bag=%d original_bag=%d projection_host=%d",
                        extension_bag, original_bag, projection_host);
            return;
        }
        if (virtual_bag::valid_original_transaction_bag(original_bag)) {
            g_original_current_direct = static_cast<uint8_t>(original_bag);
            g_original_current_got = static_cast<uint8_t>(original_bag);
            if (virtual_bag::click(&g_virtual_bag_state, extension_bag) !=
                virtual_bag::ClickResult::kIgnored) {
                install_module_view_locked(extension_bag);
                persist_state_locked();
                VIRTBAG_LOG("extension tab selected bag=%d original_bag=%d", extension_bag,
                            original_bag);
            }
        } else {
            VIRTBAG_LOG("extension tab reject original window reason=invalid_transaction_domain bag=%d",
                        original_bag);
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
        // 二次点击 = 原版袋信息面板（原版语义：desc_type=1 + UIEquip_MakeDesc）。
        // MakeDesc 读控件物品（data[0]=袋物品对象）生成详情，SetDescMenu 按
        // desc_type=1 加解除按钮；关闭详情经 UIDesc_SetOff。
        if (virtual_bag::click(&g_virtual_bag_state, extension_bag) ==
            virtual_bag::ClickResult::kInspected) {
            play_extension_switch_sound();
            void* ctrl = g_extension_tab_buttons[extension_bag];
            if (ctrl != nullptr && g_game != nullptr &&
                g_write_port->ui.make_item_description != nullptr &&
                g_write_port->ui.set_current_bag != nullptr &&
                g_write_port->ui.set_desc_type != nullptr) {
                // 门禁掩码（P3）：UIEquip_SetDescMenu desc_type=1 分支经 GOT 槽读
                // 当前原版袋索引，0（初始袋）/5（任务袋）短路不生成解除按钮
                // （b8570-b8578）。扩展袋与该全局无关——进入前所在原版袋决定
                // 按钮有无。MakeDesc 同步尾跳 SetDescMenu（b89c0），临时把
                // direct+GOT 双字节掩码为 1 使按钮恒生成，返回后立即恢复快照
                // （成对写约定同 set_original_bag_locked；按钮在 panel+0x68，
                // 不随恢复消失，点击回调已被模块 hook 接管）。Pending 路径在这些
                // UI 回调期间释放运行时锁，并在返回后复核视图身份。
                const int saved_bag = raw_direct_bag_locked();
                const int saved_desc_type = raw_desc_type_locked();
                void* const tab_data = extension_bag_ui_data(ctrl);
                void* const described_item = tab_data != nullptr
                    ? *reinterpret_cast<void**>(tab_data) : nullptr;
                const uint8_t bag_type = g_virtual_bag_state.types[extension_bag];
                const int bag_capacity = g_virtual_bag_state.capacities[extension_bag];
                if (tab_data == nullptr || described_item == nullptr) {
                    QOL_LOG_ERROR(QolDomain::kExtensionBag,
                                  "extension bag description rejected missing control item bag=%d control=%p",
                                  extension_bag, ctrl);
                } else if (saved_bag < 0 || saved_desc_type < 0) {
                    QOL_LOG_ERROR(QolDomain::kExtensionBag,
                                  "extension bag description rejected state snapshot bag=%d current_bag=%d desc_type=%d",
                                  extension_bag, saved_bag, saved_desc_type);
                } else if (unlockable_lock == nullptr) {
                    const bool bag_set = qol::contracts::succeeded(
                        g_write_port->ui.set_current_bag(1));
                    const bool desc_set = bag_set && qol::contracts::succeeded(
                        g_write_port->ui.set_desc_type(1));
                    const bool made = desc_set && qol::contracts::succeeded(
                        g_write_port->ui.make_item_description(ctrl));
                    const bool bag_restored = saved_bag < 0 || qol::contracts::succeeded(
                        g_write_port->ui.set_current_bag(saved_bag));
                    const bool desc_restored = saved_desc_type < 0 || qol::contracts::succeeded(
                        g_write_port->ui.set_desc_type(saved_desc_type));
                    if (!bag_set || !desc_set || !made || !bag_restored || !desc_restored) {
                        QOL_LOG_ERROR(QolDomain::kExtensionBag,
                                      "extension bag description UI operation failed bag=%d bag_set=%d desc_set=%d made=%d bag_restored=%d desc_restored=%d",
                                      extension_bag, bag_set ? 1 : 0, desc_set ? 1 : 0,
                                      made ? 1 : 0, bag_restored ? 1 : 0,
                                      desc_restored ? 1 : 0);
                    } else {
                        install_extension_desc_unequip_hook_locked(extension_bag);
                    }
                } else {
                    const uint64_t generation = g_inventory_generation;
                    void* const root = g_extension_tab_root;
                    unlockable_lock->unlock();
                    const bool bag_set = qol::contracts::succeeded(
                        g_write_port->ui.set_current_bag(1));
                    const bool desc_set = bag_set && qol::contracts::succeeded(
                        g_write_port->ui.set_desc_type(1));
                    const bool made = desc_set && qol::contracts::succeeded(
                        g_write_port->ui.make_item_description(ctrl));
                    const bool bag_restored = saved_bag < 0 || qol::contracts::succeeded(
                        g_write_port->ui.set_current_bag(saved_bag));
                    const bool desc_restored = saved_desc_type < 0 || qol::contracts::succeeded(
                        g_write_port->ui.set_desc_type(saved_desc_type));
                    unlockable_lock->lock();
                    bool still_current = g_virtual_bag_enabled.load() &&
                        extension_bag_session_admitted_locked() &&
                        g_inventory_generation == generation &&
                        g_extension_tab_root == root &&
                        g_virtual_bag_state.mode == virtual_bag::Mode::kModule &&
                        g_virtual_bag_state.selected == extension_bag &&
                        g_virtual_bag_state.types[extension_bag] == bag_type &&
                        g_virtual_bag_state.capacities[extension_bag] == bag_capacity &&
                        g_tab_bag_items[extension_bag] == described_item &&
                        extension_tab_control_is_current_locked(extension_bag, ctrl);
                    if (still_current) {
                        void* const current_data = extension_bag_ui_data(ctrl);
                        still_current = current_data != nullptr &&
                            *reinterpret_cast<void**>(current_data) == described_item;
                    }
                    if (!bag_set || !desc_set || !made || !bag_restored || !desc_restored) {
                        QOL_LOG_ERROR(QolDomain::kExtensionBag,
                                      "extension bag description UI operation failed bag=%d bag_set=%d desc_set=%d made=%d bag_restored=%d desc_restored=%d",
                                      extension_bag, bag_set ? 1 : 0, desc_set ? 1 : 0,
                                      made ? 1 : 0, bag_restored ? 1 : 0,
                                      desc_restored ? 1 : 0);
                    }
                    const bool description_ready = bag_set && desc_set && made &&
                                                   bag_restored && desc_restored;
                    if (!still_current || !description_ready) {
                        if (!still_current) {
                            QOL_LOG_WARN(QolDomain::kExtensionBag,
                                         "extension bag description discarded after view changed bag=%d generation=%llu",
                                         extension_bag,
                                         static_cast<unsigned long long>(generation));
                        }
                        if (made) {
                            bool closed = false;
                            if (g_write_port->ui.description_set_off != nullptr) {
                                unlockable_lock->unlock();
                                closed = qol::contracts::succeeded(
                                    g_write_port->ui.description_set_off());
                                unlockable_lock->lock();
                            }
                            if (!closed) {
                                QOL_LOG_ERROR(QolDomain::kExtensionBag,
                                              "extension bag description close failed bag=%d",
                                              extension_bag);
                            }
                        }
                    } else {
                        install_extension_desc_unequip_hook_locked(extension_bag);
                    }
                }
            } else {
                QOL_LOG_ERROR(QolDomain::kExtensionBag,
                              "extension bag description rejected UI adapter unavailable bag=%d control=%p",
                              extension_bag, ctrl);
            }
            VIRTBAG_LOG("extension bag desc opened bag=%d", extension_bag);
        }
    }
}

void queue_extension_tab_click_locked(int extension_bag) {
    if (!g_virtual_bag_enabled.load() || !extension_bag_session_admitted_locked() ||
        !virtual_bag::valid_index(extension_bag) ||
        !extension_tab_control_is_current_locked(
            extension_bag, g_extension_tab_buttons[static_cast<size_t>(extension_bag)]) ||
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

bool extension_tab_control_is_current_locked(int index, void* control) {
    if (!g_virtual_bag_enabled.load() || !extension_bag_session_admitted_locked() ||
        !virtual_bag::valid_index(index) || control == nullptr ||
        g_extension_tab_root == nullptr || g_extension_tab_buttons[index] != control ||
        g_extension_tab_generation != g_inventory_generation ||
        extension_bag_ui_root(qol::contracts::QolUiRootKind::kEquipBagContainer) !=
            g_extension_tab_root) {
        return false;
    }
    int child_count = 0;
    if (!extension_bag_ui_child_count(g_extension_tab_root, &child_count)) return false;
    for (int child = 0; child < child_count; ++child) {
        if (extension_bag_ui_child_at(g_extension_tab_root, child) == control) {
            return valid_child_locked(g_extension_tab_root, child) == control;
        }
    }
    return false;
}

void commit_pending_extension_tab_locked(std::unique_lock<std::mutex>& lock) {
    if (!virtual_bag::valid_index(g_pending_extension_tab) ||
        !g_virtual_bag_enabled.load() || !extension_bag_session_admitted_locked() ||
        g_pending_extension_tab_generation != g_inventory_generation ||
        g_extension_tab_generation != g_inventory_generation ||
        !extension_tab_control_is_current_locked(
            g_pending_extension_tab,
            g_extension_tab_buttons[static_cast<size_t>(g_pending_extension_tab)])) {
        g_pending_extension_tab = -1;
        g_pending_extension_tab_generation = 0;
        return;
    }
    const int extension_bag = g_pending_extension_tab;
    g_pending_extension_tab = -1;
    g_pending_extension_tab_generation = 0;
    handle_pending_extension_tab_click_locked(lock, extension_bag);
}

bool module_cache_clear_preflight_locked() {
    for (int bag = 0; bag < virtual_bag::kBagCount; ++bag) {
        for (int slot = 0; slot < virtual_bag::kSlotCount; ++slot) {
            const ModuleUseState& use = g_module_object_use[bag][slot];
            if (use.active) {
                VIRTBAG_LOG("module cache clear preflight rejected active object bag=%d slot=%d item=%p generation=%llu",
                            bag, slot, use.item,
                            static_cast<unsigned long long>(use.generation));
                return false;
            }
        }
    }
    return true;
}

bool clear_module_cache_locked() {
    if (!module_cache_clear_preflight_locked()) return false;
    for (int bag = 0; bag < virtual_bag::kBagCount; ++bag) {
        for (int slot = 0; slot < virtual_bag::kSlotCount; ++slot) {
            // Scheme C objects never enter g_inven, so the module owns them
            // until an explicit transfer is implemented. Drop them on slot
            // exit instead of retaining stale pointers across saves.
            if (!free_module_object_locked(bag, slot)) {
                VIRTBAG_LOG("module cache clear aborted; retained object bag=%d slot=%d item=%p",
                            bag, slot, g_module_objects[bag][slot]);
                return false;
            }
        }
    }
    cancel_projected_drag_session_locked("module-cache-clear");
    reset_extension_desc_item_hooks_locked();
    g_extension_desc_item = nullptr;
    g_extension_desc_item_bag = -1;
    g_extension_desc_item_slot = -1;
    g_original_inventory.fill(nullptr);
    // 标签袋物品对象是 ItemPool 里的裸指针，跨存档槽位加载必须失效：游戏
    // SAVE_LoadItem 重置/回收池内存，旧指针会被新读入的存档物品复用（真机
    // 实证：返回主菜单不保存重进存档后，扩展标签/信息页显示成药水、金币）。
    // 此处只清引用不释放——对象可能已被池回收，再 ItemPool_Free 是 UAF；
    // 下次 install_extension_tab_buttons 按 types 重建新对象。
    for (void*& tab_item : g_tab_bag_items) tab_item = nullptr;
    g_original_bag_size_bag = -1;
    g_original_bag_size = 0;
    g_module_window_original_bag = -1;
    g_original_current_direct = 0;
    g_original_current_got = 0;
    g_module_view_installed = false;
    g_module_view_index = -1;
    g_item_state_dirty = false;
    g_extension_touch_capture = false;
    g_extension_drag = {};
    return true;
}

bool serialize_item_payload_locked(void* item,
                                   std::array<uint8_t, virtual_bag::kSerializedItemBuffer>* out,
                                   int* out_size) {
    return virtual_bag::save_item_payload(extension_serialize_item_payload, item, out, out_size) ==
           virtual_bag::PayloadValidation::kOk;
}

void release_temporary_item_locked(void* item) {
    if (item == nullptr) return;
    extension_bag_invalidate_item_handle(item);
    (void)extension_free_item(item);
}

void* load_item_payload_locked(const uint8_t* payload, int payload_size, const char* context) {
    const virtual_bag::ManagedLoadResult result = virtual_bag::load_item_payload_exact(
        payload, payload_size, extension_serialize_item_payload, extension_load_item_payload,
        extension_free_item_payload);
    if (result.item != nullptr) return result.item;

    VIRTBAG_LOG("payload bridge reject context=%s reason=%s validation=%s size=%d",
                context,
                virtual_bag::managed_load_failure_reason(result.failure),
                virtual_bag::payload_validation_reason(result.validation), payload_size);
    return nullptr;
}

// 容量外槽位剔除的模型层实现见 virtual_bag::discard_over_capacity（Host 可测）；
// 唯一调用点是模块级保存 prepare 中扩展背包自己的校验
// virtual_bag_module_save_validate_locked，其他路径不做任何丢弃。

// P3c 准入化：加载由 save.slot_data provider 的 SessionBound load 承担（每次 session
// 激活回调 EB 内部 codec），本函数不再执行惰性 IO（读档/清缓存/换槽重读全部移除）。
// 未加载（epoch 未登记）/降级阻断时保持空状态——扫描与物品操作由调用点经
// extension_bag_session_admitted_locked() 明确拒绝；slot 漂移由 provider 的转换
// 回调（prepare_detach + load）处理，这里不得再读 current_save_slot 兜底。
void ensure_state_loaded_locked() {
    // 准入状态由 provider 回调维护（extension_bag_session_admitted_locked）；
    // 保留函数作为全部既有调用点的稳定锚，无 IO、无游戏状态读取。
}

// ---- 跨包物品移动事务（v0.7.0）----
// 顺序固定：快照/校验目标 → 内存提交 → 变更原版 → fn_save → sidecar 成功后清理回滚日志。
// 未保存的改动不写入 sidecar；pending 仅保留当前进程内的事务状态。

bool inventory_slot_locked(int bag, int slot, void** out) {
    if (g_item_facts == nullptr || g_item_facts->physical_item_at == nullptr ||
        out == nullptr || bag < 0 || bag >= 6 || slot < 0 ||
        slot >= virtual_bag::kSlotCount) {
        return false;
    }
    qol::contracts::QolItemHandle handle{};
    if (!qol::contracts::succeeded(g_item_facts->physical_item_at(bag, slot, &handle))) {
        return false;
    }
    return extension_bag_private_item_for_handle(handle, out);
}

bool original_inventory_contains_payload_locked(const uint8_t* payload, int payload_size,
                                                int target_bag) {
    if (g_game == nullptr || payload == nullptr || extension_bag_write_api() == nullptr ||
        extension_bag_write_api()->inventory.serialize_item == nullptr ||
        payload_size <= 0 || !virtual_bag::valid_original_transaction_bag(target_bag)) {
        return false;
    }
    for (int slot = 0; slot < virtual_bag::kSlotCount; ++slot) {
        void* item = nullptr;
        if (!inventory_slot_locked(target_bag, slot, &item)) continue;
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
    if (g_game == nullptr || payload == nullptr || extension_bag_write_api() == nullptr ||
        extension_bag_write_api()->inventory.serialize_item == nullptr ||
        payload_size <= 0 || !virtual_bag::valid_original_transaction_bag(target_bag)) {
        return -1;
    }
    for (int slot = 0; slot < virtual_bag::kSlotCount; ++slot) {
        void* item = nullptr;
        if (!inventory_slot_locked(target_bag, slot, &item)) continue;
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

int original_inventory_handle_slot_locked(qol::contracts::QolItemHandle expected,
                                          int target_bag) {
    if (g_item_facts == nullptr || g_item_facts->physical_item_at == nullptr ||
        !virtual_bag::valid_original_transaction_bag(target_bag)) {
        return -1;
    }
    std::array<qol::contracts::QolItemHandle, virtual_bag::kSlotCount> handles{};
    for (int slot = 0; slot < virtual_bag::kSlotCount; ++slot) {
        qol::contracts::QolItemHandle handle{};
        if (qol::contracts::succeeded(
                g_item_facts->physical_item_at(target_bag, slot, &handle))) {
            handles[static_cast<size_t>(slot)] = handle;
        }
    }
    return qol::features::extension_bag::model::find_handle_slot(
        expected, handles.data(), handles.size());
}

// ---- P4.4 五态事务协调：三条移动路径共享的唯一推进/回滚实现 ----

uint64_t g_transaction_sequence = 0;

void generate_transaction_id_locked(char* out) {
    const uint64_t seq = ++g_transaction_sequence;
    std::snprintf(out, virtual_bag::kMaxTransactionIdChars, "p4-%llu",
                  static_cast<unsigned long long>(seq));
}

// pending-recorded 阶段：事务 pending 写入 State。durable=false：仅本进程记录。
bool txn_record_pending_locked(virtual_bag::TransactionContext* txn) {
    virtual_bag::txn_build_pending(txn);
    if (!virtual_bag::valid_pending_transfer_domain(txn->pending) ||
        !virtual_bag::valid_pending_transfer_payload(txn->pending)) {
        VIRTBAG_LOG("txn pending reject id=%s reason=invalid_domain", txn->transaction_id);
        return false;
    }
    g_virtual_bag_state.pending = txn->pending;
    extension_bag_mark_dirty_locked();
    VIRTBAG_LOG("txn stage=pending-recorded durable=false id=%s direction=%u src=%u/%u dst=%u/%u",
                txn->transaction_id, static_cast<unsigned int>(txn->direction),
                static_cast<unsigned int>(txn->src_bag),
                static_cast<unsigned int>(txn->src_slot),
                static_cast<unsigned int>(txn->dst_bag),
                static_cast<unsigned int>(txn->dst_slot));
    return true;
}

// 按失败阶段回滚：先还原逻辑槽与 pending，再由调用方释放仍归模块所有的临时对象；
// 已移交原版库存的对象不参与（所有权账本终态）。
uint64_t isolation_now_ms_locked() {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                     std::chrono::steady_clock::now().time_since_epoch())
                                     .count());
}

// P4.5：pending 恢复失败隔离——原始袋槽值原样保留（可能越界，不正则化），
// payload 字节留作只读诊断。
void isolate_pending_locked(const char* reason, const char* detail) {
    const virtual_bag::PendingTransfer& pending = g_virtual_bag_state.pending;
    virtual_bag::IsolationRecord record{};
    record.valid = true;
    record.reason = reason;
    record.observed_at_ms = isolation_now_ms_locked();
    std::strncpy(record.transaction_id, pending.transaction_id,
                 virtual_bag::kMaxTransactionIdChars);
    record.direction = pending.direction;
    record.src_bag = pending.src_bag;
    record.src_slot = pending.src_slot;
    record.dst_bag = pending.dst_bag;
    record.dst_slot = pending.dst_slot;
    record.payload_size = pending.payload_size;
    record.payload = pending.payload;
    record.source_payload_size = pending.source_payload_size;
    record.source_payload = pending.source_payload;
    record.detail = detail;
    push_isolation(&g_virtual_bag_state, record);
}

// P4.5：ext→orig 失败隔离——扩展源逻辑物品保留（payload 字节未丢），记录
// 事务上下文与失败阶段供审计。
void isolate_ext2orig_failure_locked(const virtual_bag::TransactionContext& txn,
                                     virtual_bag::TxnStage phase, const char* reason,
                                     const char* detail) {
    virtual_bag::IsolationRecord record{};
    record.valid = true;
    record.reason = reason;
    record.observed_at_ms = isolation_now_ms_locked();
    std::strncpy(record.transaction_id, txn.transaction_id,
                 virtual_bag::kMaxTransactionIdChars);
    record.direction = txn.direction;
    record.phase = static_cast<int>(phase);
    record.src_bag = txn.src_bag;
    record.src_slot = txn.src_slot;
    record.dst_bag = txn.dst_bag;
    record.dst_slot = txn.dst_slot;
    record.payload_size = txn.source.payload_size;
    record.payload = txn.source.payload;
    record.detail = detail;
    push_isolation(&g_virtual_bag_state, record);
}

void txn_abort_locked(const virtual_bag::TransactionContext& txn,
                      virtual_bag::TxnStage failed_at, const char* reason) {
    virtual_bag::txn_rollback_logical(&g_virtual_bag_state, txn, failed_at);
    extension_bag_mark_dirty_locked();
    VIRTBAG_LOG("txn abort stage=%d id=%s reason=%s direction=%u src=%u/%u dst=%u/%u",
                static_cast<int>(failed_at), txn.transaction_id, reason,
                static_cast<unsigned int>(txn.direction),
                static_cast<unsigned int>(txn.src_bag),
                static_cast<unsigned int>(txn.src_slot),
                static_cast<unsigned int>(txn.dst_bag),
                static_cast<unsigned int>(txn.dst_slot));
}

void recover_pending_transaction_locked(std::unique_lock<std::mutex>& lock) {
    const virtual_bag::PendingTransfer pending = g_virtual_bag_state.pending;
    if (!pending.valid) return;
    if (!virtual_bag::valid_pending_transfer_domain(pending)) {
        isolate_pending_locked(virtual_bag::isolation_reason::kInvalidTransactionDomain,
                               "pending recovery");
        VIRTBAG_LOG("pending isolated reason=invalid_transaction_domain direction=%u src=%u/%u dst=%u/%u",
                    static_cast<unsigned int>(pending.direction),
                    static_cast<unsigned int>(pending.src_bag),
                    static_cast<unsigned int>(pending.src_slot),
                    static_cast<unsigned int>(pending.dst_bag),
                    static_cast<unsigned int>(pending.dst_slot));
        g_virtual_bag_state.pending = {};
        extension_bag_mark_dirty_locked();
        persist_state_locked();
        return;
    }
    if (!virtual_bag::valid_pending_transfer_payload(pending)) {
        const virtual_bag::PayloadValidation payload_validation =
            virtual_bag::validate_serialized_payload_buffer(pending.payload.data(),
                                                            pending.payload.size(),
                                                            pending.payload_size);
        const virtual_bag::PayloadValidation source_validation =
            pending.source_payload_size == 0
                ? virtual_bag::PayloadValidation::kOk
                : virtual_bag::validate_serialized_payload_buffer(
                      pending.source_payload.data(), pending.source_payload.size(),
                      pending.source_payload_size);
        isolate_pending_locked(virtual_bag::isolation_reason::kInvalidPayload,
                               payload_validation != virtual_bag::PayloadValidation::kOk
                                   ? virtual_bag::payload_validation_reason(payload_validation)
                                   : virtual_bag::payload_validation_reason(source_validation));
        VIRTBAG_LOG("pending isolated reason=invalid_payload payload=%s source_payload=%s src=%u/%u dst=%u/%u",
                    virtual_bag::payload_validation_reason(payload_validation),
                    virtual_bag::payload_validation_reason(source_validation),
                    static_cast<unsigned int>(pending.src_bag),
                    static_cast<unsigned int>(pending.src_slot),
                    static_cast<unsigned int>(pending.dst_bag),
                    static_cast<unsigned int>(pending.dst_slot));
        g_virtual_bag_state.pending = {};
        extension_bag_mark_dirty_locked();
        persist_state_locked();
        return;
    }
    const bool restore_module = g_module_view_installed;
    const int restore_bag = g_module_view_index;
    const int restore_original_bag = g_module_window_original_bag;
    const virtual_bag::Mode restore_mode = g_virtual_bag_state.mode;
    const int restore_selected = g_virtual_bag_state.selected;
    const int restore_inspected = g_virtual_bag_state.inspected;
    if (restore_module) restore_module_view_locked();
    const virtual_bag::RecoveryAction action =
        virtual_bag::recovery_action(g_virtual_bag_state, pending);
    VIRTBAG_LOG("pending recovery direction=%d action=%d", static_cast<int>(pending.direction),
                static_cast<int>(action));
    bool clear_pending = false;
    if (pending.direction == virtual_bag::kTransferExtensionToOriginal) {
        const bool source_matches = virtual_bag::item_matches_payload(
            g_virtual_bag_state.items[pending.src_bag][pending.src_slot], pending);
        const int target_slot = original_inventory_payload_slot_locked(
            pending.payload.data(), pending.payload_size, pending.dst_bag);
        const auto recovery = virtual_bag::ext2orig_recovery_action(
            source_matches, target_slot >= 0);
        if (recovery == virtual_bag::Ext2OrigRecoveryAction::kRollbackTarget) {
            void* target_item = nullptr;
            const bool target_read = inventory_slot_locked(pending.dst_bag, target_slot,
                                                           &target_item);
            if (target_read && target_item != nullptr &&
                remove_item_direct_unlocked(lock, pending.dst_bag, target_slot, target_item,
                                            "pending ext2orig rollback target")) {
                if (g_game != nullptr && g_write_port->ui.refresh_item_area != nullptr) {
                    virtual_bag_call_original_refresh_item_area_with_guard();
                }
                clear_pending = true;
            } else {
                VIRTBAG_LOG("pending recovery: ext2orig target rollback not confirmed; retain pending slot=%d",
                            target_slot);
            }
        } else if (recovery == virtual_bag::Ext2OrigRecoveryAction::kRollback ||
                   recovery == virtual_bag::Ext2OrigRecoveryAction::kComplete) {
            clear_pending = true;
        } else {
            set_original_bag_locked(pending.dst_bag);
            uint32_t item_handle = 0;
            void* item = load_item_payload_tracked_locked(
                pending.payload.data(), pending.payload_size,
                "recover_pending_extension_to_original", &item_handle);
            bool loaded = item != nullptr;
            if (loaded && extension_bag_write_api() != nullptr &&
                extension_bag_write_api()->inventory.save_item_on_empty != nullptr) {
                if (!extension_save_item_on_empty(item, pending.dst_bag)) {
                    if (original_inventory_contains_payload_locked(
                            pending.payload.data(), pending.payload_size, pending.dst_bag)) {
                        handover_tracked_item_locked(item_handle, item,
                                                     "recover ext2orig failed-write target observed");
                        loaded = false;
                    } else {
                        release_tracked_item_locked(item_handle, item,
                                                    "recover ext2orig insert failed");
                        loaded = false;
                    }
                }
            } else if (item != nullptr) {
                release_tracked_item_locked(item_handle, item,
                                            "recover ext2orig save unavailable");
                loaded = false;
            }
            if (loaded && !original_inventory_contains_payload_locked(
                               pending.payload.data(), pending.payload_size, pending.dst_bag)) {
                VIRTBAG_LOG("pending recovery: target payload not confirmed; retain pending");
                loaded = false;
            }
            if (loaded) {
                handover_tracked_item_locked(item_handle, item,
                                             "recover ext2orig handover");
                clear_pending = true;
            } else if (original_inventory_contains_payload_locked(
                           pending.payload.data(), pending.payload_size, pending.dst_bag)) {
                // 插入结果为假但目标载荷已可见时，目标已接管；不再释放该对象。
                clear_pending = true;
            } else {
                VIRTBAG_LOG("pending recovery: extension->original retry retained");
            }
        }
    } else {
        clear_pending = action == virtual_bag::RecoveryAction::kRollback;
    }
    if (pending.direction != virtual_bag::kTransferExtensionToOriginal &&
        action == virtual_bag::RecoveryAction::kComplete) {
        if (pending.direction == virtual_bag::kTransferOriginalToExtension) {
            void* src = nullptr;
            if (!inventory_slot_locked(pending.src_bag, pending.src_slot, &src) || src == nullptr) {
                clear_pending = true;
            } else if (pending.source_payload_size > 0 &&
                       extension_bag_write_api() != nullptr &&
                       extension_bag_write_api()->inventory.remove_item_direct != nullptr) {
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
                        const bool source_removed = remove_item_direct_unlocked(
                            lock, pending.src_bag, pending.src_slot, src, "pending recovery");
                        if (g_game != nullptr &&
                            g_write_port->ui.refresh_item_area != nullptr) {
                            virtual_bag_call_original_refresh_item_area_with_guard();
                        }
                        if (source_removed) {
                            clear_pending = true;
                        } else {
                            VIRTBAG_LOG("pending recovery: source removal not confirmed; retain pending");
                            clear_pending = false;
                        }
                    } else {
                        VIRTBAG_LOG("pending recovery: source identity mismatch; retain pending");
                    }
                }
            }
        } else if (pending.direction == virtual_bag::kTransferExtensionToExtension) {
            // ext→ext 恒纯逻辑（§4.1）：此分支 = dst 槽 payload 不匹配，仅损坏/手工
            // 构造的 sidecar 可达。pending.payload 是 dst 提交态而非可入原版库存的
            // 源载荷，禁止按 ext→orig 重放（§4.2 不尝试重放或推断目标）；
            // 按 kRollback 语义仅清 pending。
            clear_pending = true;
        }
    }
    if (clear_pending) {
        g_virtual_bag_state.pending = {};
        extension_bag_mark_dirty_locked();
    }
    // 恢复操作临时卸载投影；在退出前按原宿主和袋重新装载，避免 mode 与 UI 投影分离。
    if (restore_module) {
        const int host_bag = virtual_bag::valid_original_transaction_bag(restore_original_bag)
                                 ? restore_original_bag
                                 : 0;
        set_original_bag_locked(host_bag);
        const bool installed = virtual_bag::valid_index(restore_bag) &&
                               install_module_view_locked(restore_bag) &&
                               g_module_view_installed &&
                               g_module_view_index == restore_bag &&
                               g_projected_item_root != nullptr;
        if (installed) {
            g_virtual_bag_state.mode = restore_mode;
            g_virtual_bag_state.selected = restore_selected;
            g_virtual_bag_state.inspected = restore_inspected;
        } else {
            if (g_module_view_installed) restore_module_view_locked();
            QOL_LOG_ERROR(QolDomain::kExtensionBag,
                          "ERROR pending recovery could not restore module projection bag=%d host=%d",
                          restore_bag, host_bag);
            g_virtual_bag_state.mode = virtual_bag::Mode::kOriginal;
            g_virtual_bag_state.selected = host_bag;
            g_virtual_bag_state.inspected = -1;
        }
    }
    persist_state_locked();
}


void* materialize_module_item_locked(const virtual_bag::Item& descriptor) {
    return load_item_payload_locked(descriptor.payload.data(), descriptor.payload_size,
                                    "materialize_module_item");
}

::qol::contracts::QolItemCountEncoding category_is_equip(int category) {
    if (g_game == nullptr || g_game->read_only.item.count_encoding == nullptr || category <= 0) {
        return ::qol::contracts::QolItemCountEncoding::kUnknown;
    }
    ::qol::contracts::QolItemCountEncoding encoding =
        ::qol::contracts::QolItemCountEncoding::kUnknown;
    return qol::contracts::succeeded(
               g_game->read_only.item.count_encoding(category, &encoding))
        ? encoding
        : ::qol::contracts::QolItemCountEncoding::kUnknown;
}

// 原版 ButtonEquipExe 的背包身份判定（反汇编 b7c74）：ITEMCLASSBASE 记录
// +2 == 0x1f 即背包类。category 只是 ITEMDATABASE 索引，不能承担装备语义；
// +2 是记录字段偏移（非 VMA），表基址/步长由宿主读取面提供。
// G_ITEMCLASS_DATA/SIZE GOT 槽。
bool category_is_extension_backpack(int category) {
    if (g_game == nullptr || g_game->read_only.item.is_backpack_category == nullptr ||
        category <= 0) return false;
    int32_t value = 0;
    return qol::contracts::succeeded(
               g_game->read_only.item.is_backpack_category(category, &value)) && value != 0;
}

bool persist_state_locked(bool force) {
    (void)force;
    // The bool reports acceptance of the in-memory mutation for callers' rollback decisions;
    // it does not report durable staging. slot_data capture/complete persists state in the
    // core.save transaction, and force cannot bypass that boundary.
    return true;
}

bool extension_tab_hit(int index, int64_t x, int64_t y) {
    if (!virtual_bag::valid_index(index)) return false;
    if (g_virtual_bag_state.capacities[index] == 0) {
        // 未装备袋只有在当前拖动物品满足装备规则时可命中；否则仍保持
        // G-12 的不可命中门禁，避免普通物品误落到未装备标签。
        const int category = drag_source_category_locked();
        if (!is_backpack_category(category)) return false;
    }
    // 标签位置是动态换算的（tab_rel + 袋容器绝对），用控件真实绝对 rect 判断，
    // 不用旧硬编码 kCellX/kCellY（标签左移后二者偏移 36px 致命中失效）。
    void* ctrl = g_extension_tab_buttons[index];
    if (ctrl == nullptr) return false;
    UiRect rect{};
    return extension_bag_ui_rect(ctrl, &rect) && rect.w > 0 && rect.h > 0 &&
           extension_bag_ui_hit_test(ctrl, x, y, rect);
}

// ---- G-8 投影拖动状态机（模块侧，独立于 TouchHandle 内部拖动）----
// press 只读记录命中扩展槽（不触碰 TouchHandle）；move 更新触点 + 吞 panel 层事件
// （抑制 TouchHandle 拖动与场景溢出）；drop 在控件层路由到既有跨包事务：
//   落到物品控件（0x02）→ ext→ext；落到袋控件（0x04）→ ext→orig（经 SaveItemOnEmpty 门禁）。
// G-6：投影命中走控件 AbsoluteRect。GetAbsoluteRect 是 x8 sret 函数禁止 C++ 直调
// （真机 SIGSEGV），用手工父链累加（ctrl_abs_point 同款，纯内存读）。
// w/h 用贴图固定尺寸 kGridCell（GetAbsoluteRect 也只输出 x/y 两个 i64）。
// 物品指针合法性：模块缓存、INVEN_pItem、实时角色装备槽三集合任一命中即合法。
// 装备槽必须实时读取，不能缓存对象指针；卸下或重建后的旧指针仍由 G-6 门禁阻断。
// （借出对象不再真释放，无需隔离区）。
bool item_pointer_known_locked(void* item) {
    if (item == nullptr) return true;  // 空指针由调用方各自处理
    for (int bag = 0; bag < virtual_bag::kBagCount; ++bag) {
        for (int slot = 0; slot < virtual_bag::kSlotCount; ++slot) {
            if (g_module_objects[bag][slot] == item) return true;
        }
    }
    if (g_item_facts != nullptr && g_item_facts->physical_item_at != nullptr) {
        for (int idx = 0; idx < 6 * virtual_bag::kSlotCount; ++idx) {
            void* physical = nullptr;
            if (inventory_slot_locked(idx / virtual_bag::kSlotCount,
                                      idx % virtual_bag::kSlotCount, &physical) && physical == item) {
                return true;
            }
        }
    }
    for (int role = 0; role < 3; ++role) {
        void* character = nullptr;
        if (g_game == nullptr || g_character_extras->read.party_member == nullptr ||
            !qol::contracts::succeeded(
                g_character_extras->read.party_member(role, &character))) continue;
        if (character == nullptr) continue;
        for (int slot = 0; slot < 10; ++slot) {
            void* equipped = nullptr;
            if (g_character_extras->read.equipped_item == nullptr ||
                !qol::contracts::succeeded(
                    g_character_extras->read.equipped_item(character, slot, &equipped))) {
                continue;
            }
            if (equipped == item) return true;
        }
    }
    return false;
}

// 扩展标签绘制。挂在 DrawInvenBag wrapper（原版袋标签之后、DrawMovingItem 之前）
// 以避免遮挡拖动中的物品。绘制顺序对齐原版 UIEquip_Draw：
//   DrawInvenBackground 底框倒序(5→0) → DrawInvenBag 图标+高亮正序(0→5)。

#undef make_desc_equip_gate
#undef consume_extension_item_after_native_locked
#undef clear_original_desc_locked
#undef original_bag_locked
#undef set_original_bag_locked
#undef bag_size_word_locked
#undef set_bag_size_word_locked
#undef original_bag_button_index
#undef install_module_view_locked
#undef ensure_module_view_root_current_locked
#undef restore_module_view_locked
#undef refresh_projection_if_overwritten_locked
#undef refresh_projected_module_view_after_move_locked
#undef refresh_module_item_area_locked
#undef sync_projected_slot_locked
#undef module_item_locked
#undef prepare_main_menu_locked
#undef ensure_state_loaded_locked
#undef persist_state_locked
#undef log_exit_trace_locked
#undef delete_extension_tab_buttons_locked
#undef disable_extension_tab_buttons_locked
#undef install_extension_tab_buttons_locked
#undef extension_tab_buttons_complete_locked
#undef free_module_object_locked
#undef materialize_module_item_locked
#undef release_temporary_item_locked
#undef module_object_generation_advance_locked
#undef inventory_slot_locked
#undef log_ownership_audit_locked
#undef drain_deferred_frees_locked
#undef item_pointer_known_locked
#undef teardown_extension_bag_hosts_locked
#undef module_cache_clear_preflight_locked
#undef clear_module_cache_locked
#undef commit_pending_extension_tab_locked
#undef handle_bag_drop_release_locked
#undef route_deferred_extension_to_original_locked
#undef route_projected_session_drop_locked
#undef reset_extension_desc_unequip_hook_locked
#undef reset_extension_desc_item_hooks_locked
#undef restore_extension_desc_popup_callbacks_locked
#undef move_original_to_extension_locked
#undef move_original_to_extension_slot_locked
#undef move_extension_to_original_locked
#undef move_extension_to_extension_locked
#undef route_projected_session_to_tab_locked
#undef module_slot_is_assignable_locked
#undef module_object_replacement_allowed_locked
#undef extension_tab_hit
#undef serialize_item_payload_locked
#undef load_item_payload_locked
#undef generate_transaction_id_locked
#undef txn_record_pending_locked
#undef txn_abort_locked
#undef isolate_pending_locked
#undef isolate_ext2orig_failure_locked
#undef original_inventory_contains_payload_locked
#undef original_inventory_payload_slot_locked
#undef original_inventory_handle_slot_locked
#undef recover_pending_transaction_locked
#undef try_equip_on_extension_tab_drop_locked
#undef extension_source_should_equip_locked
#undef equip_extension_source_on_tab_locked
#undef remove_item_direct_unlocked

int original_bag_locked_internal() { return ::original_bag_locked(); }
void set_original_bag_locked_internal(int bag) { ::set_original_bag_locked(bag); }
bool bag_size_word_locked_internal(int bag, uint32_t* out_word) {
    return ::bag_size_word_locked(bag, out_word);
}
bool set_bag_size_word_locked_internal(int bag, uint32_t word) {
    return ::set_bag_size_word_locked(bag, word);
}
int original_bag_button_index_internal(int64_t x, int64_t y) {
    return ::original_bag_button_index(x, y);
}
bool install_module_view_locked_internal(int bag) { return ::install_module_view_locked(bag); }
void restore_module_view_locked_internal() { ::restore_module_view_locked(); }
void refresh_projected_module_view_after_move_locked_internal() {
    ::refresh_projected_module_view_after_move_locked();
}
void refresh_module_item_area_locked_internal(int index) {
    ::refresh_module_item_area_locked(index);
}
bool sync_projected_slot_locked_internal(int bag, int slot) {
    return ::sync_projected_slot_locked(bag, slot);
}
void* module_item_locked_internal(int bag, int slot) { return ::module_item_locked(bag, slot); }
bool free_module_object_locked_internal(int bag, int slot, void* use_token) {
    return ::free_module_object_locked(bag, slot, use_token);
}
bool module_slot_is_assignable_locked_internal(int bag, int slot) {
    return ::module_slot_is_assignable_locked(bag, slot);
}
bool module_object_replacement_allowed_locked_internal(int bag, int slot,
                                                       const char* context) {
    return ::module_object_replacement_allowed_locked(bag, slot, context);
}
bool move_original_to_extension_locked_internal(std::unique_lock<std::mutex>& lock,
                                                int dst_bag, void* moving_control) {
    return ::move_original_to_extension_locked(lock, dst_bag, moving_control);
}
bool move_original_to_extension_slot_locked_internal(
    std::unique_lock<std::mutex>& lock, int src_bag, int src_slot, int dst_bag,
    qol::contracts::QolItemHandle expected_item) {
    return ::move_original_to_extension_slot_locked(
        lock, src_bag, src_slot, dst_bag, expected_item);
}
bool move_extension_to_original_locked_internal(std::unique_lock<std::mutex>& lock,
                                                int src_bag, int src_slot, int target_bag) {
    return ::move_extension_to_original_locked(lock, src_bag, src_slot, target_bag);
}
bool move_extension_to_extension_locked_internal(int src_bag, int src_slot, int dst_bag,
                                                 int requested_dst_slot) {
    return ::move_extension_to_extension_locked(src_bag, src_slot, dst_bag,
                                                requested_dst_slot);
}
bool route_projected_session_to_tab_locked_internal(std::unique_lock<std::mutex>& lock,
                                                    int target_bag) {
    return ::route_projected_session_to_tab_locked(lock, target_bag);
}
void reset_extension_desc_item_hooks_locked_internal() {
    ::reset_extension_desc_item_hooks_locked();
}
void restore_extension_desc_popup_callbacks_locked_internal() {
    ::restore_extension_desc_popup_callbacks_locked();
}
bool try_equip_on_extension_tab_drop_locked_internal(std::unique_lock<std::mutex>& lock,
                                                     int index, void* moving_control) {
    return ::try_equip_on_extension_tab_drop_locked(lock, index, moving_control);
}
bool extension_source_should_equip_locked_internal(int target_bag) {
    return ::extension_source_should_equip_locked(target_bag);
}
bool equip_extension_source_on_tab_locked_internal(std::unique_lock<std::mutex>& lock,
                                                   int target_bag, int source_bag,
                                                   int source_slot) {
    return ::equip_extension_source_on_tab_locked(lock, target_bag, source_bag, source_slot);
}

uint32_t original_count_view(uint32_t field) {
    return virtual_bag_view_from_canonical(virtual_bag_canonical_from_field(field, 0), false);
}

}  // namespace

#undef current_env
#undef isolation_now_ms_locked
#undef extension_apply_stuff
#undef read_item_cumulate_count
#undef read_save_is_ok
#undef read_has_empty_mercenary_slot
#undef extension_description_set_off
#undef extension_use_item
#undef extension_make_mercenary
#undef extension_refresh_bag_area
#undef item_count_encoding
#undef original_count_view
#undef category_is_equip
#undef category_is_extension_backpack
#undef handle_extension_tab_click_locked

JNIEnv* current_env() { return current_env_internal(); }
uint64_t isolation_now_ms_locked() { return isolation_now_ms_locked_internal(); }
bool extension_apply_stuff(void* target_item, void* material) {
    return extension_apply_stuff_internal(target_item, material);
}
bool read_item_cumulate_count(const void* item, int* out_count) {
    return read_item_cumulate_count_internal(item, out_count);
}
bool read_save_is_ok() { return read_save_is_ok_internal(); }
bool read_has_empty_mercenary_slot() { return read_has_empty_mercenary_slot_internal(); }
void extension_description_set_off() { extension_description_set_off_internal(); }
bool extension_use_item(void* character, void* item, int arg, int* out_used) {
    return extension_use_item_internal(character, item, arg, out_used);
}
bool extension_make_mercenary(void* item, void** out_mercenary) {
    return extension_make_mercenary_internal(item, out_mercenary);
}
void extension_refresh_bag_area() { extension_refresh_bag_area_internal(); }
qol::contracts::QolItemCountEncoding item_count_encoding(const void* item) {
    return item_count_encoding_internal(item);
}
uint32_t original_count_view(uint32_t field) { return original_count_view_internal(field); }
qol::contracts::QolItemCountEncoding category_is_equip(int category) {
    return category_is_equip_internal(category);
}
bool category_is_extension_backpack(int category) {
    return category_is_extension_backpack_internal(category);
}
void handle_extension_tab_click_locked(int extension_bag) {
    handle_extension_tab_click_locked_internal(extension_bag);
}

uint64_t extension_bag_ui_isolation_now_ms_locked() {
    return isolation_now_ms_locked();
}
bool extension_bag_ui_equip_from_inventory(void* character, int bag, int slot, int equip_slot) {
    return extension_equip_from_inventory(character, bag, slot, equip_slot);
}
bool extension_bag_ui_add_money(int64_t amount) { return extension_add_money(amount); }
bool extension_bag_ui_minus_money(int64_t amount) { return extension_minus_money(amount); }
void extension_bag_ui_update_character_equip() { extension_update_character_equip(); }
void extension_bag_ui_description_set_off() { extension_description_set_off(); }
bool extension_bag_ui_read_item_cumulate_count(const void* item, int* out_count) {
    return read_item_cumulate_count(item, out_count);
}
bool extension_bag_ui_read_item_sell_price(const void* item, int64_t* out_price) {
    return read_item_sell_price(item, out_price);
}
bool extension_bag_ui_read_item_encoded_field(const void* item, uint32_t* out_field) {
    return read_item_encoded_field(item, out_field);
}
qol::contracts::QolItemCountEncoding extension_bag_ui_item_count_encoding(const void* item) {
    return item_count_encoding(item);
}
bool extension_bag_ui_read_char_can_equip(const void* character, const void* item) {
    return read_char_can_equip(character, item);
}
bool extension_bag_ui_read_char_find_equip_slot(const void* character, const void* item,
                                                int* out_slot) {
    return read_char_find_equip_slot(character, item, out_slot);
}
void* extension_bag_ui_read_char_equipped_item(const void* character, int slot) {
    return read_char_equipped_item(character, slot);
}
bool extension_bag_ui_use_item(void* character, void* item, int arg, int* out_used) {
    return extension_use_item(character, item, arg, out_used);
}
bool extension_bag_ui_make_mercenary(void* item, void** out_mercenary) {
    return extension_make_mercenary(item, out_mercenary);
}
bool extension_bag_ui_read_save_is_ok() { return read_save_is_ok(); }
bool extension_bag_ui_read_has_empty_mercenary_slot() {
    return read_has_empty_mercenary_slot();
}
void extension_bag_ui_play_switch_sound() { play_extension_switch_sound(); }
void extension_bag_ui_handle_tab_click_locked(int extension_bag) {
    handle_extension_tab_click_locked(extension_bag);
}

bool extension_tab_hit(int index, int64_t x, int64_t y) {
    return extension_tab_hit_internal(index, x, y);
}
bool extension_bag_runtime_is_backpack_category(int category) {
    return is_backpack_category(category);
}

void ensure_state_loaded_locked() { ensure_state_loaded_locked_internal(); }
bool persist_state_locked(bool force) { return persist_state_locked_internal(force); }
void log_exit_trace_locked(const char* phase, uint64_t event, uint64_t param,
                           uint64_t param2, int64_t x, int64_t y) {
    log_exit_trace_locked_internal(phase, event, param, param2, x, y);
}
void delete_extension_tab_buttons_locked() {
    delete_extension_tab_buttons_locked_internal();
}
void disable_extension_tab_buttons_locked() {
    disable_extension_tab_buttons_locked_internal();
}
void install_extension_tab_buttons_locked() {
    install_extension_tab_buttons_locked_internal();
}
bool extension_tab_buttons_complete_locked() {
    return extension_tab_buttons_complete_locked_internal();
}
void* materialize_module_item_locked(const virtual_bag::Item& descriptor) {
    return materialize_module_item_locked_internal(descriptor);
}
void release_temporary_item_locked(void* item) {
    release_temporary_item_locked_internal(item);
}
bool inventory_slot_locked(int bag, int slot, void** out) {
    return inventory_slot_locked_internal(bag, slot, out);
}
bool item_pointer_known_locked(void* item) { return item_pointer_known_locked_internal(item); }
void teardown_extension_bag_hosts_locked() {
    teardown_extension_bag_hosts_locked_internal();
}
bool module_cache_clear_preflight_locked() {
    return module_cache_clear_preflight_locked_internal();
}
bool clear_module_cache_locked() { return clear_module_cache_locked_internal(); }
void commit_pending_extension_tab_locked(std::unique_lock<std::mutex>& lock) {
    commit_pending_extension_tab_locked_internal(lock);
}
void reset_extension_desc_unequip_hook_locked() {
    reset_extension_desc_unequip_hook_locked_internal();
}
bool serialize_item_payload_locked(
    void* item, std::array<uint8_t, virtual_bag::kSerializedItemBuffer>* out, int* out_size) {
    return serialize_item_payload_locked_internal(item, out, out_size);
}
void* load_item_payload_locked(const uint8_t* payload, int payload_size, const char* context) {
    return load_item_payload_locked_internal(payload, payload_size, context);
}
void generate_transaction_id_locked(char* out) {
    generate_transaction_id_locked_internal(out);
}
bool txn_record_pending_locked(virtual_bag::TransactionContext* txn) {
    return txn_record_pending_locked_internal(txn);
}
void txn_abort_locked(const virtual_bag::TransactionContext& txn,
                      virtual_bag::TxnStage failed_at, const char* reason) {
    txn_abort_locked_internal(txn, failed_at, reason);
}
void isolate_pending_locked(const char* reason, const char* detail) {
    isolate_pending_locked_internal(reason, detail);
}
void isolate_ext2orig_failure_locked(const virtual_bag::TransactionContext& txn,
                                    virtual_bag::TxnStage phase, const char* reason,
                                    const char* detail) {
    isolate_ext2orig_failure_locked_internal(txn, phase, reason, detail);
}
bool original_inventory_contains_payload_locked(const uint8_t* payload, int payload_size,
                                                int target_bag) {
    return original_inventory_contains_payload_locked_internal(payload, payload_size, target_bag);
}
int original_inventory_payload_slot_locked(const uint8_t* payload, int payload_size,
                                           int target_bag) {
    return original_inventory_payload_slot_locked_internal(payload, payload_size, target_bag);
}
int original_inventory_handle_slot_locked(qol::contracts::QolItemHandle expected,
                                          int target_bag) {
    return original_inventory_handle_slot_locked_internal(expected, target_bag);
}
void recover_pending_transaction_locked(std::unique_lock<std::mutex>& lock) {
    recover_pending_transaction_locked_internal(lock);
}
bool remove_item_direct_unlocked(std::unique_lock<std::mutex>& lock, int bag, int slot,
                                 void* expected_item, const char* context) {
    return remove_item_direct_unlocked_internal(lock, bag, slot, expected_item, context);
}
