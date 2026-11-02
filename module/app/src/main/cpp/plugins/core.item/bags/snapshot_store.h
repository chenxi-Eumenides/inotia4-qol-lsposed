#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <vector>

#include "contracts/game_ports/item_port.h"

namespace qol::features::core_item::bags {

struct SlotRecord {
    int32_t slot;
    contracts::QolItemInventoryView view;
    uint32_t count;
};

struct BagSnapshot {
    int32_t space_id;
    int32_t bag;
    int32_t capacity;
    int32_t occupied_slots;
    std::vector<SlotRecord> items;
};

struct InventorySnapshot {
    uint64_t epoch;
    int64_t money;
    std::vector<BagSnapshot> bags;
};

class SnapshotStore {
public:
    SnapshotStore();

    void publish(std::shared_ptr<const InventorySnapshot> snapshot);
    std::shared_ptr<const InventorySnapshot> read() const;
    uint64_t bump_epoch();
    uint64_t epoch() const;
    void clear();

private:
    std::shared_ptr<const InventorySnapshot> snapshot_;
    std::atomic<uint64_t> epoch_;
};

SnapshotStore& inventory_snapshot_store();

}  // namespace qol::features::core_item::bags
