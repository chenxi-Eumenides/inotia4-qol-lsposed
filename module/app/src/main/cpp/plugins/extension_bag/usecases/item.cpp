#include "plugins/extension_bag/usecases/item.h"
#include "plugins/extension_bag/runtime/reentry_lock.h"

#include "plugins/extension_bag/runtime/entry_guard.h"
#include "plugins/extension_bag/runtime/session.h"
#include "plugins/extension_bag/runtime/shared.h"
#include "plugins/extension_bag/runtime/item_access.h"
#include "plugins/extension_bag/runtime/object_state_port.h"
#include "plugins/extension_bag/runtime/state.h"

namespace qol::features::extension_bag::usecases {

ClickItemResult click_item(int logical_bag, int slot) {
    if (!qol::extension_bag::extension_bag_on_main_thread()) {
        return {false, runtime_state().enabled.load() ? Error::kNotInGame : Error::kDisabled};
    }
    if (!extension_bag_ready_locked()) {
        return {false, runtime_state().enabled.load() ? Error::kNotInGame : Error::kDisabled};
    }
    if (slot < 0 || slot >= virtual_bag::kSlotCount) return {false, Error::kBadSlot};
    const int internal_bag = extension_storage_index(logical_bag);
    if (internal_bag < 0) return {false, Error::kBadBag};
    ::qol::extension_bag::ReentryLock lock;
    if (!runtime_state().enabled.load() || !extension_bag_session_admitted_locked()) {
        return {false, runtime_state().enabled.load() ? Error::kNotInGame : Error::kDisabled};
    }
    if (runtime_state().objects.bag.mode != virtual_bag::Mode::kModule) {
        return {false, Error::kNotInExtensionView};
    }
    if (runtime_state().objects.bag.selected != internal_bag) return {false, Error::kBagNotSelected};
    if (slot >= runtime_state().objects.bag.capacities[internal_bag]) {
        return {false, Error::kSlotBeyondCapacity};
    }
    const virtual_bag::Item& item = runtime_state().objects.bag.items[internal_bag][slot];
    if (item.category <= 0 || item.count <= 0) {
        runtime_state().objects.bag.inspected = -1;
        persist_state_locked();
        return {true, Error::kNone, false, logical_bag, slot, 0, {}};
    }
    runtime_state().objects.bag.inspected = slot;
    persist_state_locked();
    return {true, Error::kNone, true, logical_bag, slot,
            static_cast<int>(virtual_bag_effective_count(
                 module_object_slot_port(internal_bag, slot).item,
                static_cast<uint32_t>(item.count))), item};
}

}  // namespace qol::features::extension_bag::usecases
