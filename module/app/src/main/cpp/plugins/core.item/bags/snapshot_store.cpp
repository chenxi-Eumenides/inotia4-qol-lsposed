#include "snapshot_store.h"

namespace qol::features::core_item::bags {

SnapshotStore::SnapshotStore() : snapshot_(), epoch_(0) {}

void SnapshotStore::publish(std::shared_ptr<const InventorySnapshot> snapshot) {
    std::atomic_store_explicit(&snapshot_, snapshot, std::memory_order_release);
}

std::shared_ptr<const InventorySnapshot> SnapshotStore::read() const {
    return std::atomic_load_explicit(&snapshot_, std::memory_order_acquire);
}

uint64_t SnapshotStore::bump_epoch() {
    return epoch_.fetch_add(1, std::memory_order_relaxed) + 1;
}

}  // namespace qol::features::core_item::bags
