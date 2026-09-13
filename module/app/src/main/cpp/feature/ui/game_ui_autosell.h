#pragma once

// 自动出售 UI（auto-sell UI）：背包页入口按钮 + 只读占位面板原型。
//
// - 入口按钮：独立 ControlButton 挂到原版袋容器（与扩展背包页签同宿主），
//   绘制走 Scene_Draw_POPUP_SC_EQUIP 内 `bl UIDesc_Draw`（+0x1cc）调用点的独立
//   BL patch，不与扩展背包占用的 `bl GRPX_End`（+0x210）冲突。
// - 面板：改写 IAP 死条目 F_PANEL_UNK3_ENTER 的 5 个回调，由
//   UI_SetPopupProcessInfo(1, id) 打开、(3, 0) 关闭；面板自绘占位内容。
//
// 开关门控：入口按钮与面板均由全局开关（模块设置第 6 项 `autoSellEnabled`）
// 门控，默认关闭；开启后下一次背包页绘制时安装按钮，关闭时移除入口引用并关闭
// 已打开的面板。入口控件生命周期：容器指针变化即判定旧控件失效，绘制前强校验
// （子控件 + 类型 + ExecuteProc 身份），未通过校验绝不绘制。
//
// 后续接线（本轮不做）：关闭即保存接 autosell_store/ModuleSaveStore。
// 安装成功返回 true；未就绪返回 false（不改写任何字节）。
bool autosell_ui_install_if_ready();

// 全局开关：门控入口按钮与面板。
void autosell_ui_set_enabled(bool enabled);
bool autosell_ui_enabled();
