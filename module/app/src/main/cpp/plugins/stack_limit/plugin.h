#pragma once

#include "../../contracts/plugin.h"

// 堆叠上限插件清单入口（ARCHITECTURE.md §3.3）：999 功能组的指令 patch 资源。资源只声明，
// 安装/还原由宿主按 feature 生命周期通用执行；S2 编解码与数量策略归 Host 常驻 adapter。

namespace qol::features::stack_limit {

const contracts::PluginDescriptor& plugin_descriptor();

}  // namespace qol::features::stack_limit
