#pragma once

#include "plugins/extension_bag/runtime/state.h"

const qol::contracts::QolExtensionBagWritePort* extension_bag_write_api();
bool read_item_category(const void* item, int* out_category);
bool read_item_is_jewel(int category);
bool read_item_is_enchant_scroll(int category);
bool read_item_is_mercenary_seal(int category);
bool read_item_is_usable(int category);
bool item_is_equip(const void* item);
void* extension_create_item(int category, int arg1, int arg2, int arg3);
bool mark_native_bag_object(void* item);
bool extension_free_item(void* item);
const qol::contracts::QolItemCountApi* item_count_api();
bool read_world_state_id(int* out_state);
bool read_world_in_world();
int raw_direct_bag_locked();
int raw_got_bag_locked();
int raw_desc_type_locked();
bool extension_remove_item_direct(int bag, int slot, int* out_result);
int extension_serialize_item_payload(uint8_t* payload, void* item);
int extension_load_item_payload(const uint8_t* payload, void** out_item, int* out_consumed);
void extension_free_item_payload(void* item);
int virtual_bag_inventory_state_id();
bool inventory_slot_locked(int bag, int slot, void** out);
bool extension_bag_runtime_is_backpack_category(int category);
bool read_item_cumulate_count(const void* item, int* out_count);
bool read_save_is_ok();
bool read_has_empty_mercenary_slot();
void extension_description_set_off();
bool extension_use_item(void* character, void* item, int arg, int* out_used);
bool extension_make_mercenary(void* item, void** out_mercenary);
void extension_refresh_bag_area();
qol::contracts::QolItemCountEncoding item_count_encoding(const void* item);
uint32_t original_count_view(uint32_t field);
qol::contracts::QolItemCountEncoding category_is_equip(int category);
bool category_is_extension_backpack(int category);
