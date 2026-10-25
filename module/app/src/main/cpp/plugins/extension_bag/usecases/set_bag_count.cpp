#include "plugins/extension_bag/core/ui_state.h"
#include "plugins/extension_bag/usecases/set_bag_count.h"
#include "plugins/extension_bag/runtime/reentry_lock.h"

#include "plugins/extension_bag/model/virtual_bag_state.h"
#include "plugins/extension_bag/runtime/session.h"
#include "plugins/extension_bag/runtime/shared.h"
#include "plugins/extension_bag/runtime/state.h"

#include <algorithm>
#include <iterator>
#include <mutex>
#include <thread>

namespace qol::features::extension_bag::usecases {
namespace {

bool module_bag_index_has_runtime_data_locked(int bag) {
    if (virtual_bag::bag_has_data(runtime_state().objects.bag, bag)) return true;
    for (int slot = 0; slot < virtual_bag::kSlotCount; ++slot) {
        if (qol::extension_bag::ui_state().objects.module_objects[bag][slot] != nullptr ||
            qol::extension_bag::ui_state().objects.module_object_handles[bag][slot] != 0) {
            return true;
        }
        const ModuleUseState& use = qol::extension_bag::ui_state().objects.module_object_use[bag][slot];
        if (use.active || use.consumed || use.pending_release || use.item != nullptr ||
            use.generation != 0 || use.owner != std::thread::id{}) {
            return true;
        }
    }
    return false;
}

}  // namespace

SetBagCountResult usecase_set_bag_count(int count) {
    if (count < 1 || count > virtual_bag::kMaxExtensionBags) {
        return {false, Error::kInvalidBagCount, 0};
    }
    ::qol::extension_bag::ReentryLock lock;
    if (!extension_bag_ready_locked()) {
        return {false, runtime_state().enabled.load() ? Error::kNotInGame : Error::kDisabled, 0};
    }
    auto& state = runtime_state().objects.bag;
    if (state.pending.valid) return {false, Error::kTransactionPending, 0};
    if (qol::extension_bag::ui_state().ui.extension_drag.active || qol::extension_bag::ui_state().ui.extension_touch_capture ||
        qol::extension_bag::ui_state().ui.extension_drag_session.phase != virtual_bag::DragPhase::kIdle) {
        return {false, Error::kDragActive, 0};
    }
    const int old_count = state.bag_count;
    if (count < old_count) {
        for (int bag = count; bag < old_count; ++bag) {
            if (module_bag_index_has_runtime_data_locked(bag)) return {false, Error::kBagNotEmpty, 0};
        }
    } else if (count > old_count) {
        for (int bag = old_count; bag < count; ++bag) {
            if (module_bag_index_has_runtime_data_locked(bag)) {
                return {false, Error::kInactiveBagHasRuntimeData, 0};
            }
        }
    }
    if (!virtual_bag::set_bag_count(&state, count)) {
        return {false, Error::kBagCountChangeRejected, 0};
    }
    for (int bag = std::min(old_count, count); bag < std::max(old_count, count); ++bag) {
        qol::extension_bag::ui_state().objects.module_object_use[bag] = {};
        qol::extension_bag::ui_state().objects.module_object_categories[bag] = {};
        qol::extension_bag::ui_state().objects.module_object_hashes[bag] = {};
        std::fill(std::begin(qol::extension_bag::ui_state().objects.module_object_handles[bag]),
                  std::end(qol::extension_bag::ui_state().objects.module_object_handles[bag]), 0u);
    }
    extension_bag_mark_dirty_locked();
    if (!persist_state_locked()) return {false, Error::kPersistFailed, 0};
    return {true, Error::kNone, count};
}

}  // namespace qol::features::extension_bag::usecases
