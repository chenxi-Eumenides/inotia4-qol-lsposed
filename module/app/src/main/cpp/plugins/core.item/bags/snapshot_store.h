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
    contracts::QolBagInfo info;
};

struct InventorySnapshot {
    uint64_t epoch;
    uint32_t c18_bag_count;
    std::vector<BagSnapshot> bags;
    std::vector<std::pair<BagSnapshot, std::vector<SlotRecord>>> contents;
};

class SnapshotStore {
public:
    SnapshotStore();

    void publish(std::shared_ptr<const InventorySnapshot> snapshot);
    std::shared_ptr<const InventorySnapshot> read() const;
    uint64_t bump_epoch();

private:
    std::shared_ptr<const InventorySnapshot> snapshot_;
    std::atomic<uint64_t> epoch_;
};

}  // namespace qol::features::core_item::bags
