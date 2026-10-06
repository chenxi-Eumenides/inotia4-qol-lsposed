#pragma once

#include "../../contracts/game_ports/choice_port.h"
#include "../../contracts/plugin.h"

// core.custom_ui 工具/框架插件清单入口（ARCHITECTURE.md §7.1；ADR 0001 D1 的 A 档）：
//   · provides `ui_components` v1.0（可复用界面组件能力）；
//   · ui_components v1.8 提供受控原生 Options 胶囊开关与滚动行借用入口；列表拥有子开关控件树。
//   · provides `ui_panels` v1.0（面板注册/打开/占用原因）；
//   · provides `ui_choice` v1.0（原生选择控件；随控件迁入，能力 ID/版本不变）；
//   · 原生选择控件的 2 条私有资源（hook:UIChoice_Init、ptrslot:...button_list_exe_got）。

namespace qol::features::core_ui {

const contracts::PluginDescriptor& plugin_descriptor();
const contracts::QolHostSdk* host_sdk();
const contracts::QolChoicePort* choice_port();

}  // namespace qol::features::core_ui
