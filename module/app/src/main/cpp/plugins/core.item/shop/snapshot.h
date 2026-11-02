#pragma once

#include <cstdint>

#include "plugins/core.item/runtime/context.h"

namespace qol::features::core_item::shop {

inline constexpr uint32_t kSnapshotCapacity = 32768;

struct ShopSnapshot {
    uint64_t epoch = 0;
    SnapshotState state = SnapshotState::kUnavailable;
    uint32_t json_size = 0;
    char json[kSnapshotCapacity]{};
};

extern Snapshot<ShopSnapshot> g_snapshot;

bool initialize_snapshot(const contracts::QolHostSdk* sdk);
void clear_snapshot();
void destroy_snapshot();
void refresh_snapshot();

}  // namespace qol::features::core_item::shop
