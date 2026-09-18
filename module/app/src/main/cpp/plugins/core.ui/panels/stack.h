#pragma once

#include "contracts/panels.h"

// core.ui 面板栈 + `ui_panels` 能力实现（ARCHITECTURE.md §7.1）。
//
// 逻辑栈深 4，记录已打开的 panel_id（栈顶=最后打开）。provider 从 infra/panels 注册表按
// panel_id 查找；栈只编排 open/close 顺序，不解释面板语义。
//
// 语义：
//   · open 已在栈顶且 provider 自报 active → 幂等成功；
//   · open 失败（provider.open 返回 false）→ 关栈顶后重试一次（「关栈顶再开」兜底）；
//   · provider 可能自行关闭（游戏内返回键）而不经本栈：栈在每次 open 前先剔除
//     provider.active() 为假的陈旧栈顶，避免误判幂等。
//
// 线程纪律：open/close/active/block_reason 由游戏主线程或 HTTP 线程调用；provider 回调
// （open/close/active）自行保证其线程纪律（面板 provider 的 UI 操作只在主线程）。

namespace qol::features::core_ui {

constexpr int kPanelStackDepth = 4;

// `UiPanelsApi` 实现：经 infra/panels 注册表分发。
bool ui_panels_open(const char* panel_id);
void ui_panels_close(const char* panel_id);
bool ui_panels_active(const char* panel_id);

// 占用原因：优先返回栈顶仍处于 active 的 provider 的 block_reason；都不占用返回 nullptr。
// 供存档/进档等 API 守卫（verbatim 文案由各 provider 提供）。
const char* ui_panels_block_reason();

// 测试用：清空栈（不触碰 infra/panels 注册表）。
void ui_panels_reset_for_test();
// 测试用：当前栈深。
int ui_panels_depth_for_test();

}  // namespace qol::features::core_ui
