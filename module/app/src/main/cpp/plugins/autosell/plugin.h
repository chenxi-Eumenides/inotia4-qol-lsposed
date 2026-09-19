#pragma once

#include "contracts/plugin.h"

// 自动出售插件清单入口（ARCHITECTURE.md §3.3）：零资源常驻插件。
// 门禁经配置事件 `plugin.autosell.enabled` 驱动扫描任务与 UI 入口（见 plugin.cpp）。

namespace qol::features::autosell {

const contracts::PluginDescriptor& plugin_descriptor();
const contracts::QolHostSdk* host_sdk();

}  // namespace qol::features::autosell
