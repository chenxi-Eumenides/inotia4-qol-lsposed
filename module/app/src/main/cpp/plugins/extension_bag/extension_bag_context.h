#pragma once

#include <jni.h>

#include <cstdint>
#include <string>

#include "contracts/slot_data.h"
#include "contracts/ui.h"
#include "contracts/result.h"
#include "plugins/extension_bag/model/virtual_bag_state.h"

JavaVM* g_jvm();

// 操作响应信封由宿主实现；插件只依赖稳定的结果构造函数声明。
std::string op_ok();
std::string op_err(const char* msg);

// 控件矩形使用契约层类型；插件内既有代码以短名 `UiRect` 引用（与其它插件一致）。
using UiRect = qol::contracts::UiRect;

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
QolResult extension_bag_api_put_jewel_typed(int role, int logical_bag, int slot,
                                            int equip_slot);

// P3c：容器 IO/事务面已收口到 save.slot_data（见 extension_bag_persistence.cpp）；
// 旧 JNI prepare/commit/abort 桥已删除，EB 不再依赖 ModuleSaveStore/Coordinator。
bool extension_bag_session_admitted_locked();
void extension_bag_flush_dirty_staging(const qol::contracts::QolSlotDataApi* api);
bool extension_bag_slot_data_register(qol::contracts::QolOwnerToken owner,
                                      const qol::contracts::QolSlotDataApi* api);
qol::contracts::QolSlotDataRequest extension_bag_slot_data_unregister(
    const qol::contracts::QolSlotDataApi* api);
bool extension_bag_slot_data_request_settled(const qol::contracts::QolSlotDataApi* api,
                                             qol::contracts::QolSlotDataRequest request);
