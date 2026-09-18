#pragma once

#include "contracts/plugin.h"

// 库存行为域插件清单入口（ARCHITECTURE.md §3.3）：24 条入口 hook 资源的声明式所有者。
// 资源只声明，安装/还原由宿主通用执行；handler 与 claims 表本体在本插件
// plugins/inventory_hooks/native_inventory_hook.cpp，经同目录 claims.h 引用（D2c-2a）。

namespace qol::features::inventory_hooks {

const contracts::PluginDescriptor& plugin_descriptor();

}  // namespace qol::features::inventory_hooks
