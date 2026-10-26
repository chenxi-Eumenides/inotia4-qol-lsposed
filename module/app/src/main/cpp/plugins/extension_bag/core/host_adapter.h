#pragma once

#include "plugins/extension_bag/model/bag_result.h"
#include "plugins/extension_bag/runtime/state.h"
#include "plugins/extension_bag/runtime/reentry_lock.h"

void* extension_menu_character();
ExtensionBagEquipResult equip_extension_bag_item_locked(::qol::extension_bag::ReentryLock& lock,
                                                          int internal_bag, void* item);
bool equip_module_item_on_character_locked(::qol::extension_bag::ReentryLock& lock,
                                            void* character, void* item, int source_bag,
                                            int source_slot);
void finish_extension_equip_ui_locked(int internal_bag, void* moving_control);
int first_empty_extension_bag_locked();
bool extension_confirm_use_item(void* item);
bool extension_use_mercenary_seal();
bool extension_handle_original_bag_unequip();
bool extension_handle_backpack_equip();
bool consume_extension_item_after_native_locked(int bag, int slot, void* item,
                                                int before_count, int observed_before,
                                                const char* context, void* raw_use_token);
void extension_bag_ui_play_switch_sound();
ExtensionBagUnequipResult unequip_extension_bag_locked(
    ::qol::extension_bag::ReentryLock& lock, int internal_bag);
bool store_module_view_installed_locked();
void store_teardown_locked();
void mix_teardown_locked();
