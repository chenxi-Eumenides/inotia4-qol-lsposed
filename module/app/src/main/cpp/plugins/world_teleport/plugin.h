#pragma once

#include "../../contracts/plugin.h"
#include "../../contracts/game_ports/world_action_port.h"
#include "../../contracts/item_facts.h"
#include "../../contracts/static_data.h"
#include "../../contracts/world_facts.h"

// 世界传送插件清单入口（ARCHITECTURE.md §3.3）。入口为 UIPlay_CallMapName 的函数级
// kEntryHook（宿主 hooks/ 引擎安装）；变体序列经 QolGameHost.target.query_variant 读取。

namespace qol::features::world_teleport {

const contracts::PluginDescriptor& plugin_descriptor();
const contracts::QolHostSdk* host_sdk();
const contracts::QolGameHost* game_host();
const contracts::QolWorldActionPort* world_action_port();
const contracts::WorldFactsApi* world_facts();
const contracts::ItemFactsApi* item_facts();
const contracts::QolStaticDataApi* static_data();

}  // namespace qol::features::world_teleport
