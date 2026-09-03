#include "core/native/extension_bag_port.h"

#include "feature/extension_bag/game_ui_virtbag.h"

void extension_bag_sync_projected_slot(int bag, int slot) {
    virtual_bag_sync_projected_slot(bag, slot);
}

void extension_bag_sync_projected_bag() {
    virtual_bag_sync_projected_bag();
}

bool extension_bag_module_view_installed() {
    return virtual_bag_module_view_installed();
}

std::string extension_bag_inventory_bags_json() {
    return virtual_bag_inventory_bags_json();
}

bool extension_bag_is_inventory_enter(uintptr_t enter) {
    return virtual_bag_is_inventory_enter(enter);
}

void extension_bag_prepare_main_menu() {
    virtual_bag_prepare_main_menu();
}

int extension_bag_inventory_state_id() {
    return virtual_bag_inventory_state_id();
}

bool extension_bag_save_game() {
    return virtual_bag_save_game();
}

void extension_bag_prepare_save_slot_load() {
    virtual_bag_prepare_save_slot_load();
}
