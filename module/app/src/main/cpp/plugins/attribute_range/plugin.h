#pragma once

#include "contracts/plugin.h"

// 属性显示范围插件清单入口（ARCHITECTURE.md §3.3）：3 条入口 hook 资源 + attr_range 能力。
// 资源只声明，安装/还原由宿主通用执行。

namespace qol::features::attribute_range {

const contracts::PluginDescriptor& plugin_descriptor();

}  // namespace qol::features::attribute_range
