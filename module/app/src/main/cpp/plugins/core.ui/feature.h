#pragma once

// core.ui 出口面插件（ARCHITECTURE.md §7.1）：设置页容器 + 面板注册表 + `ui_panels` 能力。
//
// 依赖方向：plugins → contracts/infra/hooks；UI 原语经 game/access/ui_kit|ui_components
// 直依（U11 过渡：P5 冻结 adapter 后改 host 注入，见 ARCHITECTURE §2.2 core.ui 自我约束）。
//
// 本文件承载插件自身初始化与面板注册；面板实现位于 panels/settings.*。

namespace qol::features::core_ui {

// 插件启用标记：帧任务只在 enabled 时推进「更多游戏」按钮绑定。
bool plugin_enabled();
void plugin_set_enabled(bool enabled);

// 注册设置页 provider + 「存档备份」动作项（install）；反注册（deactivate）。
bool panels_register();
void panels_unregister();

}  // namespace qol::features::core_ui
