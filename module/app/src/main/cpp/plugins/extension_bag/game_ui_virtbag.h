#pragma once

#include "contracts/game_host.h"
#include "contracts/inventory_projection.h"
#include "plugins/extension_bag/runtime/extension_bag_port.h"

#include <jni.h>

#include <string>
#include <cstdint>


void virtual_bag_ui_register_bridge(JNIEnv* env, jclass bridge_class);

bool virtual_bag_module_view_installed();
bool virtual_bag_original_item_input_blocked();
bool virtual_bag_allow_original_tab_drop(void* control, void* source_control,
                                         uint64_t caller_offset);
bool set_virtual_bag_enabled(bool enabled);
bool virtual_bag_enabled();
bool virtual_bag_is_inventory_enter(uintptr_t enter);
bool virtual_bag_prepare_main_menu();
bool virtual_bag_sync_projected_slot(int display_bag, int slot);
bool virtual_bag_sync_projected_bag();
bool virtual_bag_sync_projected_item_control(void* control);

enum class VirtualBagProjectionDropResult : uint8_t {
    kNotExtension = static_cast<uint8_t>(qol::contracts::MoveMergeProjectionDropResult::kNotHandled),
    kHandled = static_cast<uint8_t>(qol::contracts::MoveMergeProjectionDropResult::kHandled),
    kRejected = static_cast<uint8_t>(qol::contracts::MoveMergeProjectionDropResult::kRejected),
};
VirtualBagProjectionDropResult virtual_bag_projection_drop_to_slot(void* dst_control,
                                                                    void* src_control);

using VirtualBagEquipControlEventBackup = uint64_t (*)(void*, uint64_t, void*, void*);
enum class VirtualBagEquipControlEventResult : uint8_t {
    kNotExtension = 0,
    kHandled = 1,
    kBlocked = 2,
};
VirtualBagEquipControlEventResult virtual_bag_handle_equip_control_event(
    void* control, uint64_t event, void* x2, void* param,
    VirtualBagEquipControlEventBackup backup, uint64_t* out_result);

// P5.1 read-only protocol observation. These functions never retain or log native pointers.
// The opaque token only correlates one proc invocation's pre/source/post observations.
uint64_t virtual_bag_observe_item_proc_pre(void* control, uint64_t event, void* x2, void* param);
void virtual_bag_observe_item_proc_source(uint64_t observation_token, void* control, uint64_t event,
                                          void* x2, void* param, void* source_control);
void virtual_bag_observe_item_proc_post(uint64_t observation_token, void* control, uint64_t event,
                                        void* x2, void* param, uint64_t result);


// 调用方必须已持有 g_virtual_bag_mtx；事件层与函数层共用此 g_module_objects 反查。
bool module_slot_of_item_locked(void* item, int* out_bag, int* out_slot);
void virtual_bag_call_original_refresh_item_area_with_guard();
