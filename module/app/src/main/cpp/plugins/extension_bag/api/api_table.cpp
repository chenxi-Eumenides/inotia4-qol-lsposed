#include "plugins/extension_bag/extension_bag_port.h"
#include "plugins/extension_bag/extension_bag_bridge.h"
#include "plugins/extension_bag/extension_bag_context.h"
#include "plugins/extension_bag/game_ui_virtbag.h"
#include "plugins/extension_bag/model/virtual_bag_state.h"
#include "plugins/extension_bag/model/item_handle_custody.h"
#include "plugins/extension_bag/api/port_internal.h"
#include "contracts/inventory_extension.h"
#include "contracts/sell_price.h"
#include "contracts/host_sdk.h"

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <string>

namespace qol::features::extension_bag {
namespace {

qol::contracts::ExtensionBagEquipButtonResult api_handle_backpack_button_equip_result() {
    return static_cast<qol::contracts::ExtensionBagEquipButtonResult>(
        static_cast<uint8_t>(virtual_bag_handle_backpack_button_equip_result()));
}

qol::contracts::ExtensionBagEquipEventResult api_handle_equip_control_event(
    void* control, uint64_t event, void* x2, void* param,
    qol::contracts::ExtensionBagEquipEventBackup backup, uint64_t* out_result) {
    return static_cast<qol::contracts::ExtensionBagEquipEventResult>(
        static_cast<uint8_t>(virtual_bag_handle_equip_control_event(
            control, event, x2, param, reinterpret_cast<VirtualBagEquipControlEventBackup>(backup),
            out_result)));
}

bool api_capture_move_item_observation(qol::contracts::QolItemHandle handle,
                                       int32_t target_bag, int32_t target_slot,
                                       qol::contracts::ExtensionBagMoveObservation* out) {
    if (out == nullptr) return false;
    void* item = nullptr;
    if (!extension_bag_private_item_for_handle(handle, &item)) return false;
    VirtualBagMoveItemObservation observation{};
    if (!virtual_bag_capture_move_item_observation(item, target_bag, target_slot, &observation)) {
        return false;
    }
    out->extension_item = observation.extension_item;
    out->extension_bag = observation.extension_bag;
    out->extension_slot = observation.extension_slot;
    out->extension_handle = observation.extension_handle;
    out->ownership_state = observation.ownership_state;
    out->module_view_index = observation.module_view_index;
    out->drag_session_active = observation.drag_session_active;
    out->source_bag = observation.source_bag;
    out->source_slot = observation.source_slot;
    out->source_item = qol::contracts::kInvalidItemHandle;
    out->target_item = qol::contracts::kInvalidItemHandle;
    if (observation.source_ptr != nullptr &&
        !extension_bag_item_handle_for_pointer(observation.source_ptr, &out->source_item)) {
        return false;
    }
    if (observation.target_ptr != nullptr &&
        !extension_bag_item_handle_for_pointer(observation.target_ptr, &out->target_item)) {
        return false;
    }
    out->physical_digest = observation.physical_digest;
    out->physical_nonnull = observation.physical_nonnull;
    out->physical_valid = observation.physical_valid;
    return true;
}

bool api_handle_confirm_use_item(qol::contracts::QolItemHandle handle) {
    void* item = nullptr;
    return extension_bag_private_item_for_handle(handle, &item) &&
           virtual_bag_handle_confirm_use_item(item);
}

thread_local qol::contracts::ExtensionBagPutJewelBackup g_put_jewel_backup = nullptr;
thread_local qol::contracts::QolItemHandle g_put_jewel_equip_handle{};
thread_local qol::contracts::QolItemHandle g_put_jewel_material_handle{};

int put_jewel_backup_handle_bridge(void*, void*) {
    return g_put_jewel_backup == nullptr
               ? 3
               : g_put_jewel_backup(g_put_jewel_equip_handle, g_put_jewel_material_handle);
}

int32_t api_put_jewel_native(qol::contracts::QolItemHandle equip_handle,
                             qol::contracts::QolItemHandle jewel_handle,
                             qol::contracts::ExtensionBagPutJewelBackup backup) {
    void* equip_item = nullptr;
    void* jewel_item = nullptr;
    if (!extension_bag_private_item_for_handle(equip_handle, &equip_item) ||
        !extension_bag_private_item_for_handle(jewel_handle, &jewel_item)) return 3;
    auto* previous_backup = g_put_jewel_backup;
    const auto previous_equip = g_put_jewel_equip_handle;
    const auto previous_jewel = g_put_jewel_material_handle;
    g_put_jewel_backup = backup;
    g_put_jewel_equip_handle = equip_handle;
    g_put_jewel_material_handle = jewel_handle;
    const int result = virtual_bag_put_jewel_native(
        equip_item, jewel_item, &put_jewel_backup_handle_bridge);
    g_put_jewel_backup = previous_backup;
    g_put_jewel_equip_handle = previous_equip;
    g_put_jewel_material_handle = previous_jewel;
    return result;
}

QolResult api_put_jewel(int32_t role, int32_t logical_bag, int32_t slot, int32_t equip_slot) {
    return extension_bag_api_put_jewel_typed(role, logical_bag, slot, equip_slot);
}

const qol::contracts::ExtensionBagApi kExtensionBagApi{
    // 1 状态/索引
    &provider_enabled,
    &provider_module_view_installed,
    &provider_is_inventory_enter,
    &provider_is_logical_bag,
    &provider_internal_index,
    &provider_prepare_main_menu,
    &provider_prepare_save_slot_load,
    &provider_inventory_state_id,
    &provider_for_each_logical_item,
    // 2 对象反查
    &provider_item_at,
    &provider_view_item_at,
    &provider_identify_native_item,
    &provider_find_native_item,
    &provider_remove_native_item,
    // 3 投影
    &provider_sync_projected_slot,
    &provider_sync_projected_bag,
    &provider_store_restore_for_original_write,
    // 4 收编/消耗
    &provider_consume_native_item,
    &provider_consume_category,
    &provider_has_empty_slots,
    &provider_adopt_unequipped_item,
    &provider_adopt_native_item,
    &provider_merge_native_item,
    &provider_sell_price,
    &provider_equip_projected_item,
    // 5 交互接管
    &api_handle_backpack_button_equip_result,
    &api_handle_confirm_use_item,
    &virtual_bag_handle_use_mercenary_seal,
    &api_handle_equip_control_event,
    &provider_handle_original_bag_unequip,
    &api_put_jewel_native,
    // 6 观测/拖放
    &api_capture_move_item_observation,
    // 7 内部标记
    &provider_begin_internal_equip,
    &provider_end_internal_equip,
    &provider_internal_equip_active,
    &virtual_bag_native_call_active,
    &virtual_bag_refresh_item_area_with_gate,
    // 8 移动写面
    &provider_move_item,
    // 9 typed inventory view support
     &provider_logical_bag_capacity,
     // 10 typed jewel transaction
     &api_put_jewel,
     // v3.1 sale removal operation
     &provider_remove_for_sale,
};

}  // namespace

const qol::contracts::ExtensionBagApi* extension_bag_api_table() {
    return &kExtensionBagApi;
}

}  // namespace qol::features::extension_bag
