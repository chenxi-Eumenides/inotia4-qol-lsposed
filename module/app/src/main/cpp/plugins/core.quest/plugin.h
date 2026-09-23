#pragma once

#include "../../contracts/plugin.h"
#include "../../contracts/world_facts.h"
#include "../../contracts/game_ports/quest_port.h"

// 任务完成度进档重算插件清单入口（ARCHITECTURE.md §3.3）：零资源、常驻。
// 帧节拍走 Host SDK scheduler，进档边沿经 Game Host world 状态识别。

namespace qol::features::core_quest {

const contracts::PluginDescriptor& plugin_descriptor();
const contracts::WorldFactsApi* world_facts();
const contracts::QolQuestPort* quest_port();
bool post_set_state(uint32_t quest_id, int32_t state);

}  // namespace qol::features::core_quest
