#include "plugins/extension_bag/usecases/item.h"
#include "plugins/extension_bag/runtime/reentry_lock.h"

#include "game_ui_virtbag.h"
#include "plugins/extension_bag/extension_bag_context.h"
#include "plugins/extension_bag/runtime/entry_guard.h"
#include "plugins/extension_bag/runtime/session.h"
#include "plugins/extension_bag/runtime/shared.h"
#include "plugins/extension_bag/runtime/state.h"

#include <mutex>

namespace qol::features::extension_bag::usecases {

std::string click_item(int logical_bag, int slot) {
    if (!qol::extension_bag::extension_bag_on_main_thread()) {
        return extension_bag_not_ready_error_locked_bridge();
    }
    if (!extension_bag_ready_locked_bridge()) return extension_bag_not_ready_error_locked_bridge();
    if (slot < 0 || slot >= virtual_bag::kSlotCount) return op_err("bad slot");
    const int internal_bag = extension_internal_bag_bridge(logical_bag);
    if (internal_bag < 0) return op_err("bad extension bag (6-10)");
    ::qol::extension_bag::ReentryLock lock;
    if (!runtime_state().enabled.load() || !extension_bag_session_admitted_locked()) {
        return extension_bag_not_ready_error_locked_bridge();
    }
    ensure_state_loaded_locked();
    if (runtime_state().objects.bag.mode != virtual_bag::Mode::kModule) {
        return op_err("not in extension view");
    }
    if (runtime_state().objects.bag.selected != internal_bag) return op_err("bag not selected");
    if (slot >= runtime_state().objects.bag.capacities[internal_bag]) {
        return op_err("slot beyond derived capacity");
    }
    const virtual_bag::Item& item = runtime_state().objects.bag.items[internal_bag][slot];
    if (item.category <= 0 || item.count <= 0) {
        runtime_state().objects.bag.inspected = -1;
        persist_state_locked();
        return "{\"ok\":true,\"item\":null}";
    }
    runtime_state().objects.bag.inspected = slot;
    persist_state_locked();
    return "{\"ok\":true,\"item\":{\"bag\":" + std::to_string(logical_bag) +
           ",\"slot\":" + std::to_string(slot) +
           ",\"category\":" + std::to_string(item.category) +
           ",\"count\":" +
           std::to_string(static_cast<int>(virtual_bag_effective_count(runtime_state().objects.module_objects[internal_bag][slot], static_cast<uint32_t>(item.count)))) +
           ",\"payload\":\"" +
           virtual_bag::base64_encode(item.payload.data(), item.payload_size) + "\"}}";
}

}  // namespace qol::features::extension_bag::usecases
