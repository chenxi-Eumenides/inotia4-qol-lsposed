#pragma once

#include <string>
#include <cstdint>

// Optional extension-bag projection port used by the API inventory view.
// The API layer depends on this stable port, not on extension-bag internals.
void extension_bag_sync_projected_slot(int bag, int slot);
void extension_bag_sync_projected_bag();
bool extension_bag_module_view_installed();
std::string extension_bag_inventory_bags_json();
bool extension_bag_is_inventory_enter(uintptr_t enter);
void extension_bag_prepare_main_menu();
int extension_bag_inventory_state_id();
bool extension_bag_save_game();
void extension_bag_prepare_save_slot_load();
