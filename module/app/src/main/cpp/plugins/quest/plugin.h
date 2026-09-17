#pragma once

#include "contracts/plugin.h"

// 任务完成度进档重算插件清单入口（ARCHITECTURE.md §3.3）：零资源、常驻。
// 帧节拍走 infra::scheduler；save_enter 设施迁移延后，install 直接注册 legacy 回调。

namespace qol::features::quest {

const contracts::PluginDescriptor& plugin_descriptor();

}  // namespace qol::features::quest
