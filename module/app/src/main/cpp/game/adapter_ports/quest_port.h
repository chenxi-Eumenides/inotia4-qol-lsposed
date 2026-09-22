#pragma once

#include "contracts/game_ports/quest_port.h"

namespace qol::game {

const contracts::QolQuestPort& quest_port();

// 启动期由 Kotlin 下发静态 enrichment 片段表；payload 为行式文本，nullptr 表示忽略。
void quest_port_load_static_fragments(const char* payload);

}  // namespace qol::game
