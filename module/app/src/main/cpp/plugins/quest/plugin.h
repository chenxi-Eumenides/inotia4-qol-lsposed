#pragma once

#include "../../contracts/plugin.h"

// 任务完成度进档重算插件清单入口（ARCHITECTURE.md §3.3）：零资源、常驻。
// 帧节拍走 Host SDK scheduler，进档边沿经 Game Host world 状态识别。

namespace qol::features::quest {

const contracts::PluginDescriptor& plugin_descriptor();

}  // namespace qol::features::quest
