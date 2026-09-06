#pragma once

#include <string>
#include <cstdint>

using LogicalInventoryItemFn = bool (*)(int bag, int slot, int category, int count, void* ctx);

// Optional extension-bag projection port used by the API inventory view.
// The API layer depends on this stable port, not on extension-bag internals.
void extension_bag_sync_projected_slot(int bag, int slot);
void extension_bag_sync_projected_bag();
bool extension_bag_module_view_installed();
bool extension_bag_enabled();
std::string extension_bag_inventory_bags_json();
bool extension_bag_is_inventory_enter(uintptr_t enter);
bool extension_bag_is_logical_bag(int bag);
void extension_bag_prepare_main_menu();
int extension_bag_inventory_state_id();
void extension_bag_prepare_save_slot_load();
void extension_bag_for_each_logical_item(LogicalInventoryItemFn fn, void* ctx);
void* extension_bag_item_at(int bag, int slot);
bool extension_bag_identify_native_item(void* item, int* out_bag, int* out_slot);
void* extension_bag_find_native_item(int category);
bool extension_bag_remove_native_item(void* item);
int64_t extension_bag_sell_price(void* item, int count, bool apply_variant_discount = true);
bool extension_bag_equip_projected_item(void* item, int source_bag, int source_slot, int equip_slot);
void extension_bag_begin_internal_equip();
void extension_bag_end_internal_equip();
bool extension_bag_internal_equip_active();
bool extension_bag_consume_native_item(void* item);
bool extension_bag_has_empty_slots(int needed, int include_task_bag);
std::string extension_bag_use_item(int bag, int slot);
std::string extension_bag_move_item(int from_bag, int from_slot, int to_bag, int to_slot);
std::string extension_bag_put_jewel(int role, int bag, int slot, int equip_slot);
