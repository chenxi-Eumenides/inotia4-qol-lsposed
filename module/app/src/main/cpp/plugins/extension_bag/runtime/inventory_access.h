#pragma once

#include "plugins/extension_bag/runtime/state.h"

namespace qol::contracts {
struct WorldFactsApi;
struct SaveFactsApi;
}

const qol::contracts::QolExtensionBagWritePort* extension_bag_write_api();
bool extension_save_item_on_empty(void* item, int bag);
bool extension_move_item(void* item, int count, int bag, int slot, int* out_result);
bool extension_remove_item_direct(int bag, int slot, int* out_result);
bool inventory_slot_locked(int bag, int slot, void** out);

int virtual_bag_inventory_state_id();
bool read_world_state_id(int* out_state);
bool read_world_in_world();
const qol::contracts::WorldFactsApi* extension_bag_world_facts();
const qol::contracts::SaveFactsApi* extension_bag_save_facts();
void extension_bag_bind_world_facts(const qol::contracts::WorldFactsApi* facts);
void extension_bag_bind_save_facts(const qol::contracts::SaveFactsApi* facts);

int raw_direct_bag_locked();
int raw_got_bag_locked();
int raw_desc_type_locked();

void extension_refresh_item_area();
void extension_refresh_bag_area();

void virtual_bag_call_original_refresh_item_area_with_guard();
