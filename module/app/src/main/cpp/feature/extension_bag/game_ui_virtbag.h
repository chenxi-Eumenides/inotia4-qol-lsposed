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
void* virtual_bag_item_at(int bag, int slot);
// 视图门禁版：仅当 bag 处于扩展视图（控件 index 与扩展槽 1:1 对应）时物化，
// 否则返回 nullptr。供 Stage4 装备路径在原版源槽读空时兜底。
void* virtual_bag_view_item_at(int bag, int slot);

// Thread-safe read-only native item lookup used by the stable extension-bag port.
bool virtual_bag_identify_native_item(void* item, int* out_bag, int* out_slot);
void* virtual_bag_find_native_item(int category);
bool virtual_bag_remove_native_item(void* item);
bool virtual_bag_equip_projected_item(void* item, int source_bag, int source_slot, int equip_slot);
bool virtual_bag_consume_native_item(void* item);
using VirtualBagPutJewelBackup = int (*)(void* equip_item, void* jewel_item);
int virtual_bag_put_jewel_native(void* equip_item, void* jewel_item,
                                 VirtualBagPutJewelBackup backup);
bool virtual_bag_has_empty_slots(int needed, int include_task_bag);
bool virtual_bag_adopt_unequipped_item(void* character, int equip_slot);
// 装备按钮函数级 hook（UIEquip_ButtonEquipExe）的扩展侧分流：详情物品为
// 扩展槽背包物品时返回 true（已处理，不进原函数）；否则 false 走原版。
bool virtual_bag_handle_backpack_button_equip();
// 原版袋卸下接管（ButtonUnequipExe desc_type=1）：返回 true 表示已接管（原版
// 不再执行）；out_no_space 置位表示应弹"背包已满"（袋保持装备态）。
bool virtual_bag_handle_original_bag_unequip(bool* out_no_space);
