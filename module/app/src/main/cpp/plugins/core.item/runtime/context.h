#pragma once

#include <cstdint>

#include "contracts/game_ports/character_extras_port.h"
#include "contracts/game_ports/item_dice_write_port.h"
#include "contracts/game_ports/item_inventory_state_write_port.h"
#include "contracts/game_ports/item_money_write_port.h"
#include "contracts/game_ports/item_port.h"
#include "contracts/game_ports/move_merge_port.h"
#include "contracts/host_sdk.h"
#include "contracts/item_state.h"
#include "plugins/core.item/bags/snapshot_sdk.h"

namespace qol::features::core_item {

inline constexpr const char* kPluginId = "core.item";
inline constexpr const char* kPluginVersion = "0.1.0";
inline constexpr uint32_t kInventoryCapacity = contracts::kQolItemStateRawInventoryCapacity;

enum class SnapshotState : uint8_t { kUnavailable, kNotInGame, kReady };

struct ItemSnapshot {
    SnapshotState state = SnapshotState::kUnavailable;
    int64_t money = 0;
    uint32_t inventory_size = 0;
    char inventory[kInventoryCapacity]{};
};

inline constexpr char kNotInGameBody[] = "{\"error\":\"not in game\"}";

extern const contracts::QolHostSdk* g_sdk;
extern const contracts::QolItemPort* g_port;
extern const contracts::QolItemMoneyWritePort* g_item_money_write;
extern const contracts::QolItemDiceWritePort* g_item_dice_write;
extern const contracts::QolItemInventoryStateWritePort* g_item_state_write;
extern const contracts::QolMoveMergePort* g_move_merge_port;
extern const contracts::QolCharacterExtrasPort* g_character_extras_port;
extern Snapshot<ItemSnapshot> g_snapshot;

void reset_ports();

}  // namespace qol::features::core_item
