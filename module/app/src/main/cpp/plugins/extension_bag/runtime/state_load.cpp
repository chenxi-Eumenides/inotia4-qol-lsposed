#include "plugins/extension_bag/runtime/session.h"

#include "plugins/extension_bag/extension_bag_context.h"
#include "plugins/extension_bag/model/virtual_bag_state.h"
#include "plugins/extension_bag/runtime/inventory_access.h"
#include "plugins/extension_bag/runtime/logging.h"
#include "plugins/extension_bag/runtime/projection_controller.h"
#include "plugins/extension_bag/runtime/shared.h"
#include "plugins/extension_bag/runtime/state.h"

#include <string>

void ensure_state_loaded_locked() {}

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

std::string extension_bag_not_ready_error_locked() {
    if (!runtime_state().enabled.load()) return op_err("extension bag disabled");
    return op_err("not in game");
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
            if (runtime_state().objects.module_objects[bag][slot] != nullptr ||
                runtime_state().objects.module_object_handles[bag][slot] != 0) {
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

namespace {

const char* recovery_action_name_locked() {
    const virtual_bag::RecoveryAction action =
        virtual_bag::recovery_action(runtime_state().objects.bag,
                                     runtime_state().objects.bag.pending);
    if (action == virtual_bag::RecoveryAction::kComplete) return "complete";
    if (action == virtual_bag::RecoveryAction::kRollback) return "rollback";
    return "none";
}

}  // namespace

std::string extension_bag_status_json_locked_bridge() {
    std::string original_capacities = "[";
    constexpr uint32_t kCapacityMask = (1u << 25) - 1u;
    for (int bag = 0; bag < 5; ++bag) {
        if (bag != 0) original_capacities.push_back(',');
        uint32_t word = 0;
        if (bag_size_word_locked(bag, &word)) {
            original_capacities += std::to_string(word & kCapacityMask);
        } else {
            original_capacities += "-1";
        }
    }
    original_capacities.push_back(']');
    const auto& state = runtime_state();
    return "{\"enabled\":" + std::string(state.enabled.load() ? "true" : "false") +
           ",\"injected\":" + std::string(state.persistence.state_entry != nullptr ? "true" : "false") +
           ",\"save_panel_hook\":" +
           std::string(state.persistence.save_panel_hook_installed ? "true" : "false") +
           ",\"extension_tab_button\":" +
           std::string(state.ui.extension_tab_buttons[0] != nullptr ? "true" : "false") +
           ",\"inventory_frame_active\":" +
           std::string(state.ui.inventory_frame_active ? "true" : "false") +
           ",\"recovery_action\":\"" + recovery_action_name_locked() + "\"" +
           ",\"bagCount\":" + std::to_string(virtual_bag::active_extension_bags()) +
           ",\"maxBagCount\":" + std::to_string(virtual_bag::kMaxExtensionBags) +
           ",\"originalCapacities\":" + original_capacities +
           ",\"state\":" + virtual_bag::state_json(state.objects.bag, true) + "}";
}

std::string extension_bag_not_ready_error_locked_bridge() {
    return extension_bag_not_ready_error_locked();
}

bool extension_bag_ready_locked_bridge() {
    return extension_bag_ready_locked();
}

int extension_internal_bag_bridge(int logical_bag) {
    return extension_internal_bag(logical_bag);
}

std::string extension_bag_view_result_json_bridge(bool ok, const char* error) {
    if (!ok) return op_err(error);
    return "{\"ok\":true,\"state\":" + extension_bag_status_json_locked_bridge() + "}";
}
