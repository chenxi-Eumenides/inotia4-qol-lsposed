#pragma once

#include "../../contracts/plugin.h"
#include "../../contracts/game_host.h"
#include "../../contracts/world_facts.h"
#include "../../contracts/item_facts.h"
#include "../../contracts/game_ports/inventory_extras_port.h"

// 自动出售插件清单入口（ARCHITECTURE.md §3.3）：资源常驻（gate=nullptr）。
// 开关为 descriptor feature `autosell.auto_sell`；生命周期驱动扫描任务与 UI 入口
// （见 plugin.cpp）。

namespace qol::features::autosell {

const contracts::PluginDescriptor& plugin_descriptor();
const contracts::QolHostSdk* host_sdk();
const contracts::QolGameHost* game_host();
const contracts::WorldFactsApi* world_facts();
const contracts::ItemFactsApi* item_facts();
const contracts::QolInventoryExtrasPort* inventory_extras();

}  // namespace qol::features::autosell
