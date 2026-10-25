#include "plugins/extension_bag/runtime/session.h"

#include "plugins/extension_bag/model/virtual_bag_state.h"
#include "plugins/extension_bag/runtime/inventory_access.h"
#include "plugins/extension_bag/runtime/logging.h"
#include "plugins/extension_bag/runtime/projection_controller.h"
#include "plugins/extension_bag/runtime/shared.h"
#include "plugins/extension_bag/runtime/state.h"

uint64_t extension_bag_isolation_now_ms() { return isolation_now_ms_locked(); }

bool persist_state_locked(bool force) {
    (void)force;
    // The bool reports acceptance of the in-memory mutation; it does not report durable staging.
    // slot_data capture/complete persists state in the core.save transaction.
    return true;
}

bool extension_bag_ready_locked() {
    return runtime_state().enabled.load() && read_world_in_world() &&
           extension_bag_session_admitted_locked();
}

int extension_internal_bag(int logical_bag) {
    return virtual_bag::extension_internal_bag(logical_bag);
}

bool virtual_bag_module_save_validate_locked() {
    virtual_bag::State& bag_state = runtime_state().objects.bag;
    virtual_bag::normalize(&bag_state);
    int discarded = 0;
    for (int bag = 0; bag < virtual_bag::active_extension_bags(); ++bag) {
        const int capacity = bag_state.capacities[bag];
        for (int slot = capacity; slot < virtual_bag::kSlotCount; ++slot) {
            const virtual_bag::Item& item = bag_state.items[bag][slot];
            if (item.category <= 0 && item.count <= 0) continue;
            if (extension_bag_module_slot_has_native_object(bag, slot)) {
                QOL_LOG_ERROR(QolDomain::kExtensionBag,
                              "save validation reject: over-capacity slot holds live object bag=%d slot=%d",
                              bag, slot);
                return false;
            }
            ++discarded;
        }
    }
    if (discarded > 0) {
        virtual_bag::discard_over_capacity(&bag_state);
        extension_bag_mark_dirty_locked();
        QOL_LOG_WARN(QolDomain::kExtensionBag,
                     "save validation discarded %d over-capacity item(s)", discarded);
    }
    return true;
}
