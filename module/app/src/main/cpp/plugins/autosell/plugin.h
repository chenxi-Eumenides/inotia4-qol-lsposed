#pragma once

#include "../../contracts/plugin.h"
#include "../../contracts/game_host.h"
#include "../../contracts/world_facts.h"
#include "../../contracts/item_facts.h"
#include "../../contracts/inventory_access.h"
#include "../../contracts/ui.h"
#include "../../contracts/game_ports/inventory_extras_port.h"
#include "../../contracts/game_ports/item_port.h"
#include <cstdint>

// 自动出售插件清单入口（ARCHITECTURE.md §3.3）：资源常驻（gate=nullptr）。
// 开关为 descriptor feature `autosell.auto_sell`；生命周期驱动扫描任务与 UI 入口
// （见 plugin.cpp）。

namespace qol::features::autosell {

inline constexpr uint32_t kResourceCount = 1;

// 面板 provider 声明数（PluginDescriptor.panel_provider_count）。
inline constexpr uint32_t kProviderCount = 1;
const contracts::PluginDescriptor& plugin_descriptor();
const contracts::QolHostSdk* host_sdk();
const contracts::QolGameHost* game_host();
const contracts::WorldFactsApi* world_facts();
const contracts::ItemFactsApi* item_facts();
const contracts::UiStateApi* ui_state();
const contracts::QolInventoryExtrasPort* inventory_extras();
const contracts::QolItemPort* item_port();
const contracts::QolInventoryAccessApi* inventory_access_api();

}  // namespace qol::features::autosell
