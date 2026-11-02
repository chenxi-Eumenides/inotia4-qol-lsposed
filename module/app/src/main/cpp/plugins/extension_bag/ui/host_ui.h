#pragma once

#include "contracts/item_facts.h"

#include <cstdint>

bool extension_bag_ui_equip_from_inventory(void* character, int bag, int slot, int equip_slot);
bool extension_bag_ui_add_money(int64_t amount);
bool extension_bag_ui_minus_money(int64_t amount);
void extension_bag_ui_update_character_equip();
void extension_bag_ui_description_set_off();
bool extension_bag_ui_read_item_cumulate_count(const void* item, int* out_count);
bool extension_bag_ui_read_item_sell_price(const void* item, int64_t* out_price);
bool extension_bag_ui_read_item_encoded_field(const void* item, uint32_t* out_field);
qol::contracts::QolItemCountEncoding extension_bag_ui_item_count_encoding(const void* item);
bool extension_bag_ui_read_char_can_equip(const void* character, const void* item);
bool extension_bag_ui_read_char_find_equip_slot(const void* character, const void* item,
                                                int* out_slot);
void* extension_bag_ui_read_char_equipped_item(const void* character, int slot);
bool extension_bag_ui_use_item(void* character, void* item, int arg, int* out_used);
bool extension_bag_ui_make_mercenary(void* item, void** out_mercenary);
bool extension_bag_ui_read_save_is_ok();
bool extension_bag_ui_read_has_empty_mercenary_slot();
void make_desc_equip_gate(void* ctrl, void* arg);
bool extension_handle_sell_button();

void store_install_tab_buttons_locked();
void store_delete_tab_buttons_locked();
void store_disable_tab_buttons_locked();
void store_commit_pending_tab_locked();
void store_draw_tab_buttons_locked();
void store_install_module_view_locked(int bag);
void store_refresh_projection_locked();
void store_restore_module_view_locked();
uint64_t store_event_wrapper(uint64_t event, uint64_t param, uint64_t param2);
void store_enter_wrapper();
void store_f3_wrapper();
int64_t store_sell_item_wrapper(int64_t displayed_price);
int64_t store_button_sell_exe_wrapper();
void store_draw_inven_bag_group_entry();
void store_make_desc_gate(void* ctrl, void* arg);
void store_draw_end_wrapper();
void store_draw_inven_bag_group_wrapper();
uint8_t store_buy_find_slot_gate(void* item, void* out_slot);
void mix_enter_wrapper();
void mix_f3_wrapper();
uint32_t mix_event_wrapper(uint64_t event, uint64_t param, uint64_t param2);
void mix_draw_end_wrapper();
void mix_draw_inven_bag_group_wrapper();
