#include "feature/extension_bag/game_ui_virtbag.h"
#include "feature/extension_bag/extension_bag_context.h"

std::string data_op_extension_bag_status_json() {
    return extension_bag_api_status_json_impl();
}

std::string data_op_extension_bag_enter_view(int logical_bag) {
    return extension_bag_api_enter_view_impl(logical_bag);
}

std::string data_op_extension_bag_exit_view() {
    return extension_bag_api_exit_view_impl();
}

std::string data_op_extension_bag_select_bag(int logical_bag) {
    return extension_bag_api_select_bag_impl(logical_bag);
}

std::string data_op_extension_bag_click_item(int logical_bag, int slot) {
    return extension_bag_api_click_item_impl(logical_bag, slot);
}

std::string data_op_extension_bag_unequip(int logical_bag) {
    return extension_bag_api_unequip_impl(logical_bag);
}

std::string data_op_extension_bag_move_item(int from_bag, int from_slot,
                                            int to_bag, int to_slot) {
    return extension_bag_api_move_item_impl(from_bag, from_slot, to_bag, to_slot);
}
