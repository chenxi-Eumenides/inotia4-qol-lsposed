#include "plugins/core.item/runtime/context.h"

namespace qol::features::core_item {

const contracts::QolHostSdk* g_sdk = nullptr;
const contracts::QolItemPort* g_port = nullptr;
const contracts::QolItemMoneyWritePort* g_item_money_write = nullptr;
const contracts::QolItemDiceWritePort* g_item_dice_write = nullptr;
const contracts::QolItemInventoryStateWritePort* g_item_state_write = nullptr;
const contracts::QolMoveMergePort* g_move_merge_port = nullptr;
const contracts::QolCharacterExtrasPort* g_character_extras_port = nullptr;
Snapshot<ItemSnapshot> g_snapshot;

void reset_ports() {
    g_port = nullptr;
    g_item_money_write = nullptr;
    g_item_dice_write = nullptr;
    g_item_state_write = nullptr;
    g_move_merge_port = nullptr;
    g_character_extras_port = nullptr;
    g_sdk = nullptr;
}

}  // namespace qol::features::core_item
