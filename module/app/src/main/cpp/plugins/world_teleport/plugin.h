#pragma once

#include "../../contracts/plugin.h"

// 世界传送插件清单入口（ARCHITECTURE.md §3.3）。入口 patch 由声明式 kCallPatch 资源交给
// 宿主安装；game/access 提供双版本指纹与尾部回跳元数据，patch/ 负责近址 thunk 与回滚。

namespace qol::features::world_teleport {

const contracts::PluginDescriptor& plugin_descriptor();
const contracts::QolHostSdk* host_sdk();
const contracts::QolGameHost* game_host();

}  // namespace qol::features::world_teleport
