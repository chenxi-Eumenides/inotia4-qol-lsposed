#pragma once

#include <cstdint>

#include "contracts/host_sdk.h"
#include "contracts/extension_bag.h"
#include "contracts/item_handle.h"
#include "contracts/inventory_extension.h"
#include "plugins/extension_bag/extension_bag_port.h"
#include "plugins/extension_bag/game_ui_virtbag.h"
#include "plugins/extension_bag/model/item_handle_custody.h"
#include "plugins/extension_bag/model/virtual_bag_state.h"

namespace qol::features::extension_bag::api::detail {

extern const qol::contracts::QolHostSdk* g_sdk;
extern qol::features::extension_bag::model::ItemHandleCustody g_private_item_custody;
extern const virtual_bag::CountPolicy kCountPolicy;

const qol::contracts::QolItemHandleApi* item_handle_api();

}  // namespace qol::features::extension_bag::api::detail

void provider_sync_projected_slot(int bag, int slot);
void provider_sync_projected_bag();
bool provider_module_view_installed();
bool provider_enabled();
bool provider_is_inventory_enter(uintptr_t enter);
bool provider_is_logical_bag(int bag);
int provider_internal_index(int logical_bag);
bool provider_prepare_main_menu();
int provider_inventory_state_id();
bool provider_prepare_save_slot_load();
void provider_for_each_logical_item(qol::contracts::ExtensionBagItemFn fn, void* ctx);
bool provider_logical_bag_capacity(int32_t logical_bag, int32_t* out_capacity);
int32_t provider_item_at(int bag, int slot, qol::contracts::QolItemHandle* out_item);
int32_t provider_view_item_at(int bag, int slot, qol::contracts::QolItemHandle* out_item);
bool provider_handle_backpack_button_equip();
bool provider_handle_original_bag_unequip(bool* out_no_space, bool* out_not_empty);
bool provider_identify_native_item(qol::contracts::QolItemHandle handle,
                                   int* out_bag, int* out_slot);
int32_t provider_find_native_item(int category, qol::contracts::QolItemHandle* out_item);
bool provider_remove_native_item(qol::contracts::QolItemHandle handle);
int64_t provider_sell_price(qol::contracts::QolItemHandle handle, int count,
                            bool apply_variant_discount);
QolResult provider_remove_for_sale(int32_t logical_bag, int32_t slot,
                                   qol::contracts::QolItemHandle expected_item);
bool provider_equip_projected_item(void* character, qol::contracts::QolItemHandle handle,
                                   int source_bag, int source_slot, int equip_slot);
void provider_begin_internal_equip();
void provider_end_internal_equip();
bool provider_internal_equip_active();
bool provider_consume_native_item(qol::contracts::QolItemHandle handle);
int provider_consume_category(int category, int count);
bool provider_has_empty_slots(int needed, int include_task_bag);
bool provider_adopt_unequipped_item(void* character, int equip_slot);
bool provider_adopt_native_item(qol::contracts::QolItemHandle handle);
bool provider_merge_native_item(qol::contracts::QolItemHandle handle);
void provider_store_restore_for_original_write();
int32_t provider_move_item(int32_t from_bag, int32_t from_slot, int32_t to_bag,
                           int32_t to_slot, char* out, uint32_t capacity,
                           uint32_t* out_size);
std::string extension_bag_inventory_bags_json();
