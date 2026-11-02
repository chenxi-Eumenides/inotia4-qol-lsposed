#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <vector>

#include "contracts/game_ports/item_port.h"

namespace qol::features::core_inventory::bags {

// 与 core.item 的同名实现保持暂存重复：inventory HTTP 读侧 owner 迁移期内两侧
// 各自持有一份快照存储；item.state 仍读 core.item 的存储，本份只服务 core.inventory
// 的 HTTP 读面。
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

}  // namespace qol::features::core_inventory::bags
