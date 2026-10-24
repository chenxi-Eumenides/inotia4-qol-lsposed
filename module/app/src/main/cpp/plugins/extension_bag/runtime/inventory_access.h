#pragma once

#include "plugins/extension_bag/runtime/state.h"

const qol::contracts::QolExtensionBagWritePort* extension_bag_write_api();
bool extension_save_item_on_empty(void* item, int bag);
bool extension_move_item(void* item, int count, int bag, int slot, int* out_result);
bool extension_remove_item_direct(int bag, int slot, int* out_result);
bool inventory_slot_locked(int bag, int slot, void** out);

int virtual_bag_inventory_state_id();
bool read_world_state_id(int* out_state);
bool read_world_in_world();

int raw_direct_bag_locked();
int raw_got_bag_locked();
int raw_desc_type_locked();

void extension_refresh_item_area();
void extension_refresh_bag_area();
