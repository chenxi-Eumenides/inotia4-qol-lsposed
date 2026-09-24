#pragma once

#include "../../contracts/plugin.h"

// character_panel_i18n 插件（ADR 0001 D4）：角色面板属性标签汉化（内容贡献者）。
// 声明面见 feature.h（标签表与消费者注册）与 charinfo_scope.h（绘制窗口作用域 hook）。

namespace qol::features::character_panel_i18n {

const contracts::PluginDescriptor& plugin_descriptor();

}  // namespace qol::features::character_panel_i18n
