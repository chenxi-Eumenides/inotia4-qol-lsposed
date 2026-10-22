#pragma once

#include "../../contracts/plugin.h"

// 弹窗屏蔽插件清单入口：通过 feature 生命周期持有教程状态入队入口 patch。

namespace qol::features::popup_blocker {

const contracts::PluginDescriptor& plugin_descriptor();

}  // namespace qol::features::popup_blocker
