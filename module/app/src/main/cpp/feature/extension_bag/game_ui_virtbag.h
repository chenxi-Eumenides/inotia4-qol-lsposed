#pragma once

#include "core/native/stack_codec.h"

#include <jni.h>

#include <string>
#include <cstdint>

#include "core/native/extension_bag_port.h"

void virtual_bag_ui_start_auto_inject();
void virtual_bag_ui_register_bridge(JNIEnv* env, jclass bridge_class);

bool virtual_bag_module_view_installed();
bool virtual_bag_original_item_input_blocked();
bool virtual_bag_allow_original_tab_drop(void* control, void* source_control,
                                         uint64_t caller_offset);
bool set_virtual_bag_enabled(bool enabled);
bool virtual_bag_enabled();
bool virtual_bag_is_inventory_enter(uintptr_t enter);
int virtual_bag_inventory_state_id();
bool virtual_bag_prepare_main_menu();
bool virtual_bag_prepare_save_slot_load();
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
enum class VirtualBagProjectionDropResult : uint8_t {
    kNotExtension,
    kHandled,
    kRejected,
};
VirtualBagProjectionDropResult virtual_bag_projection_drop_to_slot(void* dst_control,
                                                                    void* src_control);
std::string data_op_extension_bag_exit_view();
std::string data_op_extension_bag_select_bag(int logical_bag);
std::string data_op_extension_bag_click_item(int logical_bag, int slot);
std::string data_op_extension_bag_move_item(int from_bag, int from_slot, int to_bag, int to_slot);
std::string virtual_bag_inventory_bags_json();
void virtual_bag_for_each_logical_item(LogicalInventoryItemFn fn, void* ctx);
void* virtual_bag_item_at(int bag, int slot);
// 按 ITEMCLASSBASE +6 bit0 统一判定 payload 数量字段是否为 stack_codec 语义。
stack_codec::CountEncoding virtual_bag_category_uses_stack_count(int category);
// 视图门禁版：仅当 bag 处于扩展视图（控件 index 与扩展槽 1:1 对应）时物化，
// 否则返回 nullptr。供 Stage4 装备路径在原版源槽读空时兜底。
void* virtual_bag_view_item_at(int bag, int slot);

// Thread-safe read-only native item lookup used by the stable extension-bag port.
bool virtual_bag_identify_native_item(void* item, int* out_bag, int* out_slot);
// 调用方必须已持有 g_virtual_bag_mtx；事件层与函数层共用此 g_module_objects 反查。
bool module_slot_of_item_locked(void* item, int* out_bag, int* out_slot);
struct VirtualBagMoveItemObservation {
    bool extension_item = false;
    int extension_bag = -1;
    int extension_slot = -1;
    uint32_t extension_handle = 0;
    int ownership_state = 0;
    int module_view_index = -1;
    bool drag_session_active = false;
    int source_bag = -1;
    int source_slot = -1;
    void* source_ptr = nullptr;
    void* target_ptr = nullptr;
    uint64_t physical_digest = 0;
    int physical_nonnull = 0;
    bool physical_valid = false;
};
// 身份与取证数据在锁内采集；调用方必须放锁后再调用原版 backup。
bool virtual_bag_capture_move_item_observation(void* item, int target_bag, int target_slot,
                                               VirtualBagMoveItemObservation* out);
void* virtual_bag_find_native_item(int category);
bool virtual_bag_remove_native_item(void* item, void* use_token);
bool virtual_bag_equip_projected_item(void* character, void* item, int source_bag,
                                      int source_slot, int equip_slot);
bool virtual_bag_consume_native_item(void* item, void* use_token);
void* virtual_bag_current_use_token();
using VirtualBagEquipControlEventBackup = uint64_t (*)(void*, uint64_t, void*, void*);
enum class VirtualBagEquipControlEventResult : uint8_t {
    kNotExtension,
    kHandled,
    kBlocked,
};
VirtualBagEquipControlEventResult virtual_bag_handle_equip_control_event(
    void* control, uint64_t event, void* x2, void* param,
    VirtualBagEquipControlEventBackup backup, uint64_t* out_result);
using VirtualBagPutJewelBackup = int (*)(void* equip_item, void* jewel_item);
int virtual_bag_put_jewel_native(void* equip_item, void* jewel_item,
                                 VirtualBagPutJewelBackup backup);
bool virtual_bag_has_empty_slots(int needed, int include_task_bag);
bool virtual_bag_adopt_unequipped_item(void* character, int equip_slot);
// INVEN_SaveItem 无空位时把原版新物品收编进扩展袋空位。
bool virtual_bag_adopt_native_item(void* item);
// INVEN_SaveItem backup 前的扩展同类堆合并（VM-39）：仅当扩展袋已有可合并堆
// 时返回 true（并入并释放收编对象）；不新建扩展槽，找不到则返回 false 让原版
// original-first 入库。
bool virtual_bag_merge_native_item(void* item);
enum class VirtualBagEquipButtonResult : uint8_t {
    kNotExtension,
    kHandled,
    kBlocked,
};
// 装备按钮函数级 hook（UIEquip_ButtonEquipExe）的扩展侧分流：详情物品为
// 扩展槽背包物品时返回 true（已处理，不进原函数）；否则 false 走原版。
VirtualBagEquipButtonResult virtual_bag_handle_backpack_button_equip_result();
bool virtual_bag_handle_backpack_button_equip();
// 确认使用回调的扩展侧分流：item 与当前扩展详情物品及逻辑袋槽一致时返回 true
// 并接管确认流程；否则返回 false，由原版回调继续处理。
bool virtual_bag_handle_confirm_use_item(void* item);
// 原版袋卸下接管（ButtonUnequipExe desc_type=1，含扩展袋解除）：返回 true
// 表示已接管（原版不再执行）；out_no_space/out_not_empty 置位表示应弹
// "背包已满"/"袋非空"（袋保持装备态）。
bool virtual_bag_handle_original_bag_unequip(bool* out_no_space, bool* out_not_empty);
// 商店扩展视图安装期间若发生原版 INVEN 写入（买入 SaveItem 等），必须先
// 恢复商店投影（还原被放大的窗口袋容量字），否则原版 FindSaveSlot 会把
// 物品写进超过真实容量的槽位（恢复后物品丢失）。由 SaveItem hook 调用。
void virtual_bag_store_restore_for_original_write();
// 原版刷新函数级关卡：调用方不持扩展锁，由该函数负责锁内原版刷新和投影覆盖。
void virtual_bag_refresh_item_area_with_gate();
// 原版函数可能回调进入库存 Hook；持扩展锁调用已审计的原版路径时使用该 TLS 标记，
// 查询 Hook 将只走 original backup，避免回调再次获取 g_virtual_bag_mtx。
bool virtual_bag_native_call_active();
void virtual_bag_call_original_refresh_item_area_with_guard();
