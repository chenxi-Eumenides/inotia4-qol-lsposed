#pragma once

#include "contracts/plugin.h"

// 堆叠上限插件清单入口（ARCHITECTURE.md §3.3）：999 功能组的指令 patch 资源（gate =
// plugin.stack_limit.enabled）。资源只声明，安装/还原由宿主通用执行；有效态由插件内部持有。

namespace qol::features::stack_limit {

const contracts::PluginDescriptor& plugin_descriptor();

}  // namespace qol::features::stack_limit
