#pragma once

#include "plugins/extension_bag/runtime/state.h"

bool read_item_category(const void* item, int* out_category);
bool read_item_is_jewel(int category);
bool read_item_is_enchant_scroll(int category);
bool read_item_is_mercenary_seal(int category);
bool read_item_is_usable(int category);
bool item_is_equip(const void* item);
bool read_item_cumulate_count(const void* item, int* out_count);
const qol::contracts::QolItemCountApi* item_count_api();
qol::contracts::QolItemCountEncoding item_count_encoding(const void* item);
uint32_t original_count_view(uint32_t field);
qol::contracts::QolItemCountEncoding category_is_equip(int category);
bool category_is_extension_backpack(int category);
bool extension_bag_runtime_is_backpack_category(int category);

void* extension_create_item(int category, int arg1, int arg2, int arg3);
bool mark_native_bag_object(void* item);
bool extension_free_item(void* item);
bool extension_serialize_item(uint8_t* payload, int payload_capacity, void* item,
                              int* out_size);
void* extension_load_item(const uint8_t* payload, int payload_size, int* out_consumed);
int extension_serialize_item_payload(uint8_t* payload, void* item);
int extension_load_item_payload(const uint8_t* payload, void** out_item, int* out_consumed);
void extension_free_item_payload(void* item);

bool extension_use_item(void* character, void* item, int arg, int* out_used);
bool extension_make_mercenary(void* item, void** out_mercenary);
bool extension_apply_stuff(void* target_item, void* material);
bool extension_equip_from_inventory(void* character, int bag, int slot, int equip_slot);
bool extension_add_money(int64_t amount);
bool extension_minus_money(int64_t amount);

bool read_save_is_ok();
bool read_has_empty_mercenary_slot();
void extension_description_set_off();
