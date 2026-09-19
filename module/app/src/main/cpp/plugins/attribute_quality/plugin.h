#pragma once

#include "../../contracts/plugin.h"

// 属性质量 provider 插件清单入口：MATH_GetRandom 捕获资源 + attribute.quality 能力。

namespace qol::features::attribute_quality {

const contracts::PluginDescriptor& plugin_descriptor();

}  // namespace qol::features::attribute_quality
