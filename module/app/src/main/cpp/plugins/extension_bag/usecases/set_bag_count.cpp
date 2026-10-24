#include "plugins/extension_bag/usecases/set_bag_count.h"

#include "plugins/extension_bag/extension_bag_context.h"
#include "plugins/extension_bag/model/virtual_bag_state.h"
#include "plugins/extension_bag/runtime/lifecycle.h"
#include "plugins/extension_bag/runtime/shared.h"
#include "plugins/extension_bag/runtime/state.h"

#include <algorithm>
#include <iterator>
#include <mutex>
#include <string>
#include <thread>

namespace qol::features::extension_bag::usecases {
namespace {

bool module_bag_index_has_runtime_data_locked(int bag) {
    if (virtual_bag::bag_has_data(runtime_state().objects.bag, bag)) return true;
    for (int slot = 0; slot < virtual_bag::kSlotCount; ++slot) {
        if (runtime_state().objects.module_objects[bag][slot] != nullptr ||
            runtime_state().objects.module_object_handles[bag][slot] != 0) {
            return true;
        }
        const ModuleUseState& use = runtime_state().objects.module_object_use[bag][slot];
        if (use.active || use.consumed || use.pending_release || use.item != nullptr ||
            use.generation != 0 || use.owner != std::thread::id{}) {
            return true;
        }
    }
    return false;
}

}  // namespace

std::string usecase_set_bag_count(int count) {
    if (count < 1 || count > virtual_bag::kMaxExtensionBags) {
        return op_err("count required (1-8)");
    }
    std::lock_guard<std::mutex> lock(runtime_state().mtx);
    ensure_state_loaded_locked();
    if (!extension_bag_ready_locked_bridge()) return extension_bag_not_ready_error_locked_bridge();
    auto& state = runtime_state().objects.bag;
    if (state.pending.valid) return op_err("transaction pending");
    if (runtime_state().ui.extension_drag.active || runtime_state().ui.extension_touch_capture ||
        runtime_state().ui.extension_drag_session.phase != virtual_bag::DragPhase::kIdle) {
        return op_err("drag active");
    }
    const int old_count = state.bag_count;
    if (count < old_count) {
        for (int bag = count; bag < old_count; ++bag) {
            if (module_bag_index_has_runtime_data_locked(bag)) return op_err("bag not empty");
        }
    } else if (count > old_count) {
        for (int bag = old_count; bag < count; ++bag) {
            if (module_bag_index_has_runtime_data_locked(bag)) return op_err("inactive bag has runtime data");
        }
    }
    if (!virtual_bag::set_bag_count(&state, count)) return op_err("bag count change rejected");
    for (int bag = std::min(old_count, count); bag < std::max(old_count, count); ++bag) {
        runtime_state().objects.module_object_use[bag] = {};
        runtime_state().objects.module_object_categories[bag] = {};
        runtime_state().objects.module_object_hashes[bag] = {};
        std::fill(std::begin(runtime_state().objects.module_object_handles[bag]),
                  std::end(runtime_state().objects.module_object_handles[bag]), 0u);
    }
    extension_bag_mark_dirty_locked();
    if (!persist_state_locked()) return op_err("persist failed");
    return "{\"ok\":true,\"count\":" + std::to_string(count) + "}";
}

}  // namespace qol::features::extension_bag::usecases
