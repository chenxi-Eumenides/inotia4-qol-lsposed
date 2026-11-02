#pragma once

#include <mutex>

namespace qol::features::extension_bag::model {

template <typename SnapshotReader, typename LockedWork>
void with_snapshot_then_lock(std::mutex& mutex, SnapshotReader read_snapshot,
                             LockedWork locked_work) {
    const auto snapshot = read_snapshot();
    std::lock_guard<std::mutex> lock(mutex);
    locked_work(snapshot);
}

}  // namespace qol::features::extension_bag::model
