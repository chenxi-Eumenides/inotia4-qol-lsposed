#pragma once

void* extension_bag_ui_item(void* control);
void* extension_bag_ui_data(void* control);
bool extension_bag_ui_set_control_item(void* control, void* item);
void* module_item_locked(int bag, int slot);
void refresh_module_item_area_locked(int index);
void restore_module_view_locked();
void virtual_bag_call_original_refresh_item_area_with_guard();
