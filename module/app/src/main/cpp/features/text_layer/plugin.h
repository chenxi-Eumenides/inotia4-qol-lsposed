#pragma once

#include "contracts/plugin.h"

// 文本层插件清单入口（ARCHITECTURE.md §3.3）：1 条共享资源 + text_layer v1 能力。
// 资源只声明，安装/还原由宿主通用执行。

namespace qol::features::text_layer {

const contracts::PluginDescriptor& plugin_descriptor();

}  // namespace qol::features::text_layer
