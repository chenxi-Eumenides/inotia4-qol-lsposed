#pragma once

#include "contracts/game_ports/item_port.h"

namespace qol::game {

const contracts::QolItemPort& item_port();

// 启动期由 Kotlin 下发静态名称表；payload 为行式文本，nullptr 表示忽略。运行期不再写入。
void item_port_load_static_text(const char* payload);

}  // namespace qol::game
