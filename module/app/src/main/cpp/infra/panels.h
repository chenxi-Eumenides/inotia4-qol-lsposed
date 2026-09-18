#pragma once

#include <cstdint>

#include "contracts/panels.h"

// 面板注册表（ARCHITECTURE.md §3.4 / §4.1；D1-b0 前置）：纯数据面，零 game 依赖。
// 面板插件在启用时登记 provider 与设置页条目，出口面 UI 容器只读列举。
//
// 容量固定：设置页条目 32 条、provider 16 条（静态数组）。
// 线程纪律：注册/注销只在装配期/主线程发生，本模块不引入锁。

namespace qol::infra {

// 登记设置页条目：同 id 异 owner → false；同 owner 幂等 → true；
// id/owner 空 → false；容量耗尽 → false。
bool panels_register_item(const contracts::PanelItemSpec& item);

// 登记面板 provider：同 panel_id 异 owner → false；同 owner 幂等 → true；
// panel_id/owner 空 → false；容量耗尽 → false。
bool panels_register_provider(const contracts::PanelProviderSpec& provider);

// 注销该 owner 的全部条目与 provider（注册顺序保持）；owner 空 → 无操作。
void panels_unregister_owner(const char* owner);

uint32_t panels_item_count();

// index 越界返回 nullptr。
const contracts::PanelItemSpec* panels_item_at(uint32_t index);

// panel_id 未登记返回 nullptr。
const contracts::PanelProviderSpec* panels_find_provider(const char* panel_id);

// 测试用：清空注册表。
void panels_reset_for_test();

}  // namespace qol::infra
