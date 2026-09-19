#pragma once

#include "../../contracts/plugin.h"

// 文本层插件清单入口（ARCHITECTURE.md §3.3）：2 条入口 hook 资源 + text_layer v1 能力。
// - hook:MEMORYTEXT_GetText：共享资源（owner 独占，消费者经 dispatch）；
// - hook:Scene_Draw_Popup_SC_CharacterInfo：角色面板绘制窗口作用域标记（私有）。
// 资源只声明，安装/还原由宿主通用执行。

namespace qol::features::text_layer {

const contracts::PluginDescriptor& plugin_descriptor();
const contracts::QolHostSdk* host_sdk();
const contracts::QolGameHost* game_host();

}  // namespace qol::features::text_layer
