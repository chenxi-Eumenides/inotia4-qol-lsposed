#pragma once

#include "../../contracts/plugin.h"

// 简单模式插件清单入口（ARCHITECTURE.md §3.3）：装配期注册静态存储的描述符。
// 资源只声明，安装/还原由宿主通用执行。

namespace qol::features::simple_mode {

const contracts::PluginDescriptor& plugin_descriptor();

}  // namespace qol::features::simple_mode
