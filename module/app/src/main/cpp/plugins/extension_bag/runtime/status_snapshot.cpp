#include "plugins/extension_bag/runtime/status_snapshot.h"

#include "plugins/extension_bag/model/serialization.h"

#include <memory>
#include <string>

namespace qol::extension_bag {
namespace {

StatusSnapshotPtr g_status_snapshot;

const char* recovery_action_name(virtual_bag::RecoveryAction action) {
    if (action == virtual_bag::RecoveryAction::kComplete) return "complete";
    if (action == virtual_bag::RecoveryAction::kRollback) return "rollback";
    return "none";
}

}  // namespace

void publish_status_snapshot(const StatusSnapshot& value) {
    auto snapshot = std::make_shared<StatusSnapshot>(value);
    std::atomic_store_explicit(&g_status_snapshot, StatusSnapshotPtr(std::move(snapshot)),
                               std::memory_order_release);
}

StatusSnapshotPtr load_status_snapshot() {
    return std::atomic_load_explicit(&g_status_snapshot, std::memory_order_acquire);
}

std::string status_snapshot_json(const StatusSnapshot& snapshot) {
    std::string original_capacities = "[";
    for (size_t bag = 0; bag < snapshot.original_capacities.size(); ++bag) {
        if (bag != 0) original_capacities.push_back(',');
        original_capacities += std::to_string(snapshot.original_capacities[bag]);
    }
    original_capacities.push_back(']');
    return "{\"enabled\":" + std::string(snapshot.enabled ? "true" : "false") +
           ",\"injected\":" + std::string(snapshot.injected ? "true" : "false") +
           ",\"save_panel_hook\":" +
           std::string(snapshot.save_panel_hook_installed ? "true" : "false") +
           ",\"extension_tab_button\":" +
           std::string(snapshot.extension_tab_button ? "true" : "false") +
           ",\"inventory_frame_active\":" +
           std::string(snapshot.inventory_frame_active ? "true" : "false") +
           ",\"recovery_action\":\"" + recovery_action_name(snapshot.recovery_action) + "\"" +
           ",\"bagCount\":" +
           std::to_string(virtual_bag::clamp_active_extension_bags(snapshot.bag.bag_count)) +
           ",\"maxBagCount\":" + std::to_string(virtual_bag::kMaxExtensionBags) +
           ",\"originalCapacities\":" + original_capacities +
           ",\"state\":" + virtual_bag::state_json(snapshot.bag, true) + "}";
}

}  // namespace qol::extension_bag
