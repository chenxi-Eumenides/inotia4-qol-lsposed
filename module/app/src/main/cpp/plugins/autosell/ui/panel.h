#pragma once

#include <cstdint>

// 自动出售游戏内面板（背包齿轮入口按钮 + 按存档配置面板；原 feature/ui/game_ui_autosell，
// D1-b3 迁入 autosell 插件）。
//
// - 入口按钮：独立 ControlButton 挂到原版袋容器（与扩展背包页签同宿主），绘制走
//   Scene_Draw_POPUP_SC_EQUIP 内 `bl UIDesc_Draw`（+0x1cc）调用点的独立 BL patch，
//   不与扩展背包占用的 `bl GRPX_End`（+0x210）冲突；该 patch 现为插件的声明式
//   kCallPatch 资源（id=`patch:SCENE_DRAW_EQUIP+0x1cc`，handler=本文件的绘制 wrapper）。
// - 面板：通过 ui_panels 注册带回调的 provider，由 core.custom_ui 的 PopupHost 在打开时
//   托管 PopupState；面板自绘并编辑配置草稿，关闭与返回由宿主面板栈处理。
//
// 开关门控：入口按钮与面板均由插件启用态（descriptor feature `autosell.auto_sell` 的
// 运行态）门控，默认关闭；开启后下一次背包页绘制时安装按钮，关闭时移除入口引用、请求
// 关闭面板。
//
// 线程模型：入口安装/绘制、面板 enter/process/event 均在游戏主线程；开关位由 JVM/主线程
// 共享（atomic）。

namespace qol::features::autosell {

// 入口按钮绘制宿主：kCallPatch 站点 `patch:SCENE_DRAW_EQUIP+0x1cc` 的 handler。
void autosell_equip_desc_draw_wrapper();

// 全局开关：门控入口按钮与面板（配置事件/on_enabled/on_disabled 调用）。
void autosell_ui_set_enabled(bool enabled);
bool autosell_ui_enabled();

// ---- PanelProviderSpec 实现（panel_id="autosell"）----
bool autosell_provider_open();
void autosell_provider_close();
bool autosell_provider_active();
const char* autosell_provider_block_reason();

// ---- PopupHost 面板回调 ----
void autosell_panel_enter();
void autosell_panel_process();
void autosell_panel_f3();
void autosell_panel_f4();
uint32_t autosell_panel_event(uint64_t event, uint64_t param, uint64_t param2);

}  // namespace qol::features::autosell
