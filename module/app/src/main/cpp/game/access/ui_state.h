#pragma once

#include <cstdint>
#include <string>

// 游戏层：UI 读/写原语（T2l-u，迁自 api/native/game_ui_read.inc / game_ui_operations.inc）。
// 栈顶面板判定、screen 细分与面板开关游戏语义收口在本层；转场/别名反查/扩展清理分别经
// contracts/runtime_hooks.h、contracts/popup_enter_alias.h 与 game/adapter_ports 生命周期 facade。
// 线程纪律：读写均依赖 popup 栈/UI 全局，只在游戏主线程调用。

namespace qol::game {

// 栈顶面板原版 enter VMA（减去 g_base；模块 wrapper 经别名反查还原）；无面板/未就绪返回 0。
uintptr_t popup_top_vma();
// 栈顶面板名（与 panel_* 输出口径一致）；非面板/未就绪返回 nullptr。
const char* top_panel_name();

// UI 写结果（固定宽度 POD；完整 op JSON 信封）。
struct UiWriteResult {
    uint32_t size = 0;
    char text[256] = {};

    const char* data() const { return text; }
    std::string str() const { return std::string(text, size); }
};

// 主菜单：blocking（transition_run 跨帧）/ direct（transition_run_inline，owner 逻辑相位）；
// 面板守卫 + 扩展袋清理 + GAMESTATE_SetState(4)（成功 refresh 恰一次）。
UiWriteResult ui_main_menu(bool direct);
// 关闭栈顶面板（仅面板可 Pop；弹窗 fail-closed）→ UI_SetPopupProcessInfo(3,0) + HUD 恢复。
UiWriteResult ui_panel_close();
// 打开白名单面板：名→state id 扫描 → 栈顶守卫 → Pop/等待 → Push。
UiWriteResult ui_panel_open(const std::string& panel);


// ---- 对话读面（T2l-d 迁自 game_dialog_content.inc，逐字）----
// NPC 选项列表 JSON（UICHOICE count/focus/options）。
std::string npc_dialog_options_json();
// 对话内容 JSON（popup 最优先 → story → wipeout/npc_quest 面板 → NPC 对话 → 面板态 → none）。
std::string dialog_content_json();

// ---- 对话写原语（T2l-d）----
//
// 迁移自 game_dialog_operations.inc：NPC 交互/选择分发/弹窗确认/剧情推进。返回值可能含长文本
// （next 的 desc 文本），故定长 4096（非 UiWriteResult 的 256）。
struct DialogWriteResult {
    uint32_t size = 0;
    char text[4096] = {};

    const char* data() const { return text; }
    std::string str() const { return std::string(text, size); }
};

// NPC 交互：NearNPC + 触摸链兜底 → UICHOICE/任务执行；成功 refresh 恰一次。
DialogWriteResult npc_interact();
// 按 index 选择 NPC 任务列表项（NPCTASKLIST）。
DialogWriteResult npc_dialog_select(int index);
// 对话动作分发（popup/story/wipeout/npc_quest/npc/面板态；action: ok/cancel/next/skip/complete/close/game_over/save）。
DialogWriteResult dialog_select(const std::string& action, int index);
// 弹窗确认/取消（BUTTON_OK_EXE / BUTTON_CANCEL_EXE）。
DialogWriteResult dialog_ok();
DialogWriteResult dialog_cancel();
// 剧情推进/跳过（EVENT_BUTTON_OK_EXE / EVENT_BUTTON_SKIP_EXE）。
DialogWriteResult story_next();
DialogWriteResult story_skip();

}  // namespace qol::game
