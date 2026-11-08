#include "plugins/extension_bag/usecases/set_bag_count.h"
#include "plugins/extension_bag/runtime/reentry_lock.h"

#include "plugins/extension_bag/model/virtual_bag_state.h"
#include "plugins/extension_bag/runtime/session.h"
#include "plugins/extension_bag/runtime/shared.h"
#include "plugins/extension_bag/runtime/state.h"
#include "plugins/extension_bag/runtime/object_state_port.h"
#include "plugins/extension_bag/runtime/ownership_port.h"
#include "plugins/extension_bag/runtime/logging.h"
#include "plugins/extension_bag/core/projection.h"
#include "plugins/extension_bag/core/ui_state.h"

#include <mutex>

namespace qol::features::extension_bag::usecases {
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
    if (extension_drag_active_port() || extension_touch_capture_active_port() ||
        extension_drag_session_active_port()) {
        return {false, Error::kDragActive, 0};
    }
    const int old_count = state.bag_count;
    if (count == old_count) return {true, Error::kNone, count};
    auto& objects = qol::extension_bag::ui_state().objects;
    if (count < old_count && objects.module_view_installed && objects.module_view_index >= count)
        restore_module_view_locked();
    if (count < old_count) {
        bool cleanup_complete = true;
        for (int bag = count; bag < old_count; ++bag) {
            for (int slot = 0; slot < virtual_bag::kSlotCount; ++slot) {
                if (!free_module_object_locked(bag, slot)) cleanup_complete = false;
            }
        }
        if (!cleanup_complete) {
            QOL_LOG_WARN(QolDomain::kExtensionBag,
                         "bag count apply deferred reason=runtime cleanup failed old=%d requested=%d",
                         old_count, count);
            return {false, Error::kBagCountChangeRejected, 0};
        }
    }
    if (!virtual_bag::set_bag_count(&state, count)) {
        return {false, Error::kBagCountChangeRejected, 0};
    }
    extension_bag_mark_dirty_locked();
    if (!persist_state_locked()) return {false, Error::kPersistFailed, 0};
    return {true, Error::kNone, count};
}

}  // namespace qol::features::extension_bag::usecases
