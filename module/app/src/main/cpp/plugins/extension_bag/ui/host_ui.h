#pragma once

#include "plugins/extension_bag/runtime/state.h"

// Forward declarations keep the host UI surface independent of the common umbrella.
enum class ExtensionBagEquipResult;
enum class ExtensionBagUnequipResult;
void* extension_menu_character();
ExtensionBagEquipResult equip_extension_bag_item_locked(std::unique_lock<std::mutex>& lock,
                                                          int internal_bag, void* item);
bool equip_module_item_on_character_locked(std::unique_lock<std::mutex>& lock,
                                            void* character, void* item, int source_bag,
                                            int source_slot);
void finish_extension_equip_ui_locked(int internal_bag, void* moving_control);
int first_empty_extension_bag_locked();
bool extension_confirm_use_item(void* item);
bool extension_use_mercenary_seal();
void make_desc_equip_gate(void* ctrl, void* arg);
bool consume_extension_item_after_native_locked(int bag, int slot, void* item,
                                                int before_count, int observed_before,
                                                const char* context, void* raw_use_token);
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
void extension_bag_ui_play_switch_sound();
bool extension_apply_stuff(void* target_item, void* material);
ExtensionBagUnequipResult unequip_extension_bag_locked_bridge(
    std::unique_lock<std::mutex>& lock, int internal_bag);
ExtensionBagUnequipResult unequip_extension_bag_locked(
    std::unique_lock<std::mutex>& lock, int internal_bag);
bool store_module_view_installed_locked();
void store_teardown_locked();
void store_restore_module_view_locked();
void store_install_tab_buttons_locked();
void store_delete_tab_buttons_locked();
void store_disable_tab_buttons_locked();
void store_commit_pending_tab_locked();
void store_draw_tab_buttons_locked();
void store_install_module_view_locked(int bag);
void store_refresh_projection_locked();
uint64_t store_event_wrapper(uint64_t event, uint64_t param, uint64_t param2);
void store_enter_wrapper();
void store_f3_wrapper();
int64_t store_sell_item_wrapper(int64_t displayed_price);
void store_make_desc_gate(void* ctrl, void* arg);
void store_draw_end_wrapper();
void store_draw_inven_bag_group_wrapper();
uint8_t store_buy_find_slot_gate(void* item, void* out_slot);
void mix_teardown_locked();
void mix_enter_wrapper();
void mix_f3_wrapper();
void mix_draw_end_wrapper();
void mix_draw_inven_bag_group_wrapper();
