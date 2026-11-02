#include "plugins/core.item/runtime/context.h"

#include <algorithm>

#include "plugins/core.item/bags/native_bag_limit.h"

namespace qol::features::core_item {

namespace {
int g_native_bag_limit = contracts::kQolOriginalBagCount;
}

namespace bags {

int native_bag_limit() { return g_native_bag_limit; }

void set_native_bag_limit(int32_t limit) {
    g_native_bag_limit = std::clamp(limit, contracts::kQolOriginalBagCount,
                                    contracts::kQolMaxNativeBagCount);
}

}  // namespace bags

const contracts::QolHostSdk* g_sdk = nullptr;
const contracts::QolItemPort* g_port = nullptr;
const contracts::QolItemMoneyWritePort* g_item_money_write = nullptr;
const contracts::QolItemDiceWritePort* g_item_dice_write = nullptr;
const contracts::QolItemInventoryStateWritePort* g_item_state_write = nullptr;
const contracts::QolCharacterExtrasPort* g_character_extras_port = nullptr;
Snapshot<ItemSnapshot> g_snapshot;

void reset_ports() {
    g_port = nullptr;
    g_item_money_write = nullptr;
    g_item_dice_write = nullptr;
    g_item_state_write = nullptr;
    g_character_extras_port = nullptr;
    g_sdk = nullptr;
}

}  // namespace qol::features::core_item
