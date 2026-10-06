#pragma once

#include "../../../contracts/host_sdk.h"
#include "../../../contracts/panels.h"

// core.ui 面板栈 + `ui_panels` 能力实现（ARCHITECTURE.md §7.1）。
//
// 逻辑栈深 4，记录已打开的 panel_id（栈顶=最后打开）。provider 经 Host SDK 按 panel_id
// 查找；栈只编排 open/close 顺序，不解释面板语义。
//
// 语义：
//   · open 已在栈顶且 provider 自报 active → 幂等成功；
//   · open 失败（provider.open 返回 false）→ 关栈顶后重试一次（「关栈顶再开」兜底）；
//   · provider 可能自行关闭（游戏内返回键）而不经本栈：栈在每次 open 前先剔除
//     provider.active() 为假的陈旧栈顶，避免误判幂等。
//
// 线程纪律：open/close 只由游戏主线程调用；HTTP 线程须提交命令。active/block_reason
// 只查询 provider 状态，不执行游戏 UI 操作。

namespace qol::features::core_ui {

void ui_panels_bind_sdk(const contracts::QolHostSdk* sdk);

constexpr int kPanelStackDepth = 4;

// `UiPanelsApi` 实现：经 Host SDK panels 表分发。
bool ui_panels_open(const char* panel_id);
void ui_panels_close(const char* panel_id);
bool ui_panels_back();
// 延迟关闭请求：可在弹窗事件/生命周期回调（原生事件遍历中）安全调用，实际 pop/restore
// 由常驻帧任务在下一帧执行，避免在游戏控件遍历内同步销毁。多处请求同帧合并为一次。
void ui_panels_request_back();
bool ui_panels_active(const char* panel_id);
bool ui_panels_deactivate_owner(const char* owner);
int32_t ui_panels_frame_pump(int64_t frame, void* context);
// 当前逻辑栈深（只读）：自适应导航按钮绘制与调试展示用；不在 open/close 之外修改栈。
int ui_panels_depth();

// 占用原因：优先返回栈顶仍处于 active 的 provider 的 block_reason；都不占用返回 nullptr。
// 供存档/进档等 API 守卫（verbatim 文案由各 provider 提供）。
const char* ui_panels_block_reason();

// 测试用：清空栈（不触碰 Host SDK panels 注册表）。
void ui_panels_reset_for_test();
// 测试用：当前栈深。
int ui_panels_depth_for_test();
// 测试用：直接设置栈深（0..kPanelStackDepth），仅用于隔离验证导航按钮的视觉判定，
// 不触发 provider 回调、不改变任何栈行为。
void ui_panels_set_depth_for_test(int depth);

}  // namespace qol::features::core_ui
