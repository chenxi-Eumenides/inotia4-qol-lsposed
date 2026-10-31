#include "plugins/core.inventory/runtime/context.h"

#include <algorithm>

namespace qol::features::core_inventory {

const contracts::QolHostSdk* g_sdk = nullptr;
const contracts::QolItemPort* g_port = nullptr;

namespace bags {

namespace {
int g_native_bag_limit = contracts::kQolOriginalBagCount;
}

int native_bag_limit() { return g_native_bag_limit; }

void set_native_bag_limit(int32_t limit) {
    g_native_bag_limit = std::clamp(limit, contracts::kQolOriginalBagCount,
                                    contracts::kQolMaxNativeBagCount);
}

}  // namespace bags

}  // namespace qol::features::core_inventory
