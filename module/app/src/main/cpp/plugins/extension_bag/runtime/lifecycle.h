#pragma once

#include "plugins/extension_bag/runtime/state.h"

void virtual_bag_f3_wrapper();
void virtual_bag_inventory_enter_wrapper();
void virtual_bag_save_panel_enter_wrapper();
uint64_t virtual_bag_event(uint64_t event, uint64_t param, uint64_t param2);
void ensure_lifecycle_task();
void remove_lifecycle_task();
bool prepare_main_menu_locked();
void teardown_extension_bag_hosts_locked();
bool clear_module_cache_locked();
bool module_cache_clear_preflight_locked();
PhysicalInventorySnapshot capture_physical_inventory_snapshot_locked();
void ensure_state_loaded_locked();
bool persist_state_locked(bool force = false);
void* materialize_module_item_locked(const virtual_bag::Item& descriptor);
void release_temporary_item_locked(void* item);
bool virtual_bag_module_save_validate_locked();
