#include "plugins/core.inventory/runtime/context.h"

#include <algorithm>

#include "contracts/game_ports/item_port.h"

namespace qol::features::core_inventory::bags {

namespace {
int g_native_bag_limit = contracts::kQolOriginalBagCount;
}

int native_bag_limit() { return g_native_bag_limit; }

void set_native_bag_limit(int32_t limit) {
    g_native_bag_limit = std::clamp(limit, contracts::kQolOriginalBagCount,
                                    contracts::kQolMaxNativeBagCount);
}

}  // namespace qol::features::core_inventory::bags
