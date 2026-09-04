#pragma once

#include <jni.h>

#include <cstdint>
#include <string>

#include "feature/extension_bag/model/virtual_bag_state.h"

// Internal-only context accessors. They are not part of the API/core port.
JNIEnv* extension_bag_current_env();
jclass extension_bag_bridge_class();
virtual_bag::State* extension_bag_state();
uint64_t extension_bag_isolation_now_ms();

std::string extension_bag_api_status_json_impl();
std::string extension_bag_api_enter_view_impl(int logical_bag);
std::string extension_bag_api_exit_view_impl();
std::string extension_bag_api_select_bag_impl(int logical_bag);
std::string extension_bag_api_click_item_impl(int logical_bag, int slot);
std::string extension_bag_api_unequip_impl(int logical_bag);
std::string extension_bag_api_move_item_impl(int from_bag, int from_slot,
                                              int to_bag, int to_slot);
std::string extension_bag_api_use_item_impl(int logical_bag, int slot);
std::string extension_bag_api_put_jewel_impl(int role, int logical_bag, int slot,
                                             int equip_slot);

bool extension_bag_load_state_from_store(int slot);
bool extension_bag_save_state_to_store(int slot);
bool extension_bag_prepare_save_to_store(int slot, const char* transaction_id);
    bool extension_bag_commit_save_to_store(int slot, const char* transaction_id);
bool extension_bag_abort_known_failed_save(int slot, const char* transaction_id);
