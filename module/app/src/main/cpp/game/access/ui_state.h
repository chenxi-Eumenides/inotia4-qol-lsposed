#pragma once

#include <cstdint>
#include <string>

// 游戏层：UI 读/写原语（T2l-u，迁自 api/native/game_ui_read.inc / game_ui_operations.inc）。
// 栈顶面板判定、screen 细分与面板开关游戏语义收口在本层；转场/别名反查/EB 清理分别经
// contracts/runtime_hooks.h、contracts/popup_enter_alias.h、contracts/extension_bag_port.h。
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

}  // namespace qol::game
