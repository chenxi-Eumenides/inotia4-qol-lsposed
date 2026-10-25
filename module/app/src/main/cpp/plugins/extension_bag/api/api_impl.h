#pragma once

#include "contracts/result.h"

#include <string>

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
QolResult extension_bag_api_put_jewel_typed(int role, int logical_bag, int slot,
                                             int equip_slot);
