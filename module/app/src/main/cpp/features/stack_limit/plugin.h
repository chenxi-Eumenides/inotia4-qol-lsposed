#pragma once

#include "contracts/plugin.h"

// 堆叠上限插件清单入口（ARCHITECTURE.md §3.3）：6 条 clamp 指令 patch 资源（gate =
// plugin.stack_limit.enabled）。资源只声明，安装/还原由宿主通用执行；有效态经
// core/native/stack_limit_port 的 stack_limit_set_effective 写入。

namespace qol::features::stack_limit {

const contracts::PluginDescriptor& plugin_descriptor();

}  // namespace qol::features::stack_limit
