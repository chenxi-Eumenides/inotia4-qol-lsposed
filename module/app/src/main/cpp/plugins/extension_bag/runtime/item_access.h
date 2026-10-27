#pragma once

#include "plugins/extension_bag/runtime/state.h"

#include <jni.h>

JavaVM* g_jvm();

bool read_item_category(const void* item, int* out_category);
bool read_item_type(const void* item, qol::contracts::QolItemType* out_type);
int original_item_category_locked(void* item);
bool extension_bag_register_item_handle(void* item);
bool extension_bag_item_handle_for_pointer(void* item,
                                           qol::contracts::QolItemHandle* out_handle);
bool read_item_is_jewel(const void* item);
bool read_item_is_enchant_scroll(const void* item);
bool read_item_is_mercenary_seal(const void* item);
bool read_item_is_usable(int category);
bool item_is_equip(const void* item);
bool read_item_cumulate_count(const void* item, int* out_count);
const qol::contracts::QolItemCountApi* item_count_api();
qol::contracts::QolItemCountEncoding item_count_encoding(const void* item);
uint32_t original_count_view(uint32_t field);
uint32_t virtual_bag_canonical_count(const void* item, uint32_t fallback_canonical);
uint32_t virtual_bag_canonical_from_field(uint32_t field, uint32_t fallback_canonical);
uint32_t virtual_bag_effective_count(const void* item, uint32_t fallback_canonical);
uint32_t virtual_bag_field_from_canonical(uint32_t field, uint32_t canonical, bool extended);
uint32_t virtual_bag_view_from_canonical(uint32_t canonical, bool extended);
uint32_t virtual_bag_count_limit(bool extended);
bool virtual_bag_extended_mode(const void* item);
bool virtual_bag_write_effective(void* item, uint32_t count);
qol::contracts::QolItemCountEncoding category_is_equip(int category);
qol::contracts::QolItemCountEncoding virtual_bag_category_uses_stack_count(int category);
bool category_is_extension_backpack(const void* item);
bool extension_bag_runtime_is_backpack_category(const void* item);

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
