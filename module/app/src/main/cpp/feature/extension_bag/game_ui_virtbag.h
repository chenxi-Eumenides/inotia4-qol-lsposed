#pragma once

#include <jni.h>

#include <string>
#include <cstdint>

#include "core/native/extension_bag_port.h"

void virtual_bag_ui_start_auto_inject();
void virtual_bag_ui_register_bridge(JNIEnv* env, jclass bridge_class);

bool virtual_bag_module_view_installed();
bool virtual_bag_original_item_input_blocked();
bool virtual_bag_allow_original_tab_drop(void* control, void* source_control);
bool set_virtual_bag_enabled(bool enabled);
bool virtual_bag_enabled();
bool virtual_bag_is_inventory_enter(uintptr_t enter);
int virtual_bag_inventory_state_id();
void virtual_bag_prepare_main_menu();
void virtual_bag_prepare_save_slot_load();
bool virtual_bag_sync_projected_slot(int display_bag, int slot);
bool virtual_bag_sync_projected_bag();
bool virtual_bag_sync_projected_item_control(void* control);

// P5.1 read-only protocol observation. These functions never retain or log native pointers.
// The opaque token only correlates one proc invocation's pre/source/post observations.
uint64_t virtual_bag_observe_item_proc_pre(void* control, uint64_t event, void* x2, void* param);
void virtual_bag_observe_item_proc_source(uint64_t observation_token, void* control, uint64_t event,
                                          void* x2, void* param, void* source_control);
void virtual_bag_observe_item_proc_post(uint64_t observation_token, void* control, uint64_t event,
                                        void* x2, void* param, uint64_t result);

std::string data_virtual_bag_ui_status_json();
std::string data_virtual_bag_test_equip(int index, int bag_type);
std::string data_virtual_bag_test_item(int index, int slot, int category, int count);

std::string data_op_extension_bag_status_json();
std::string data_op_extension_bag_enter_view(int logical_bag);
std::string data_op_extension_bag_unequip(int logical_bag);
bool virtual_bag_projection_drop_to_slot(void* dst_control, void* src_control);
std::string data_op_extension_bag_exit_view();
std::string data_op_extension_bag_select_bag(int logical_bag);
std::string data_op_extension_bag_click_item(int logical_bag, int slot);
std::string data_op_extension_bag_move_item(int from_bag, int from_slot, int to_bag, int to_slot);
std::string virtual_bag_inventory_bags_json();
void virtual_bag_for_each_logical_item(LogicalInventoryItemFn fn, void* ctx);
