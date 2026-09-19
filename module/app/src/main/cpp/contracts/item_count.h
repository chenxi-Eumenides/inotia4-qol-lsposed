#pragma once

#include "contracts/game_host.h"

namespace qol::contracts {

// 宿主侧遗留消费者的只读访问器；provider 未驻留时返回 nullptr。
const QolItemCountApi* item_count_api();

}  // namespace qol::contracts
