#include "plugins/extension_bag/runtime/status_snapshot.h"

#include "plugins/extension_bag/runtime/projection_controller.h"
#include "plugins/extension_bag/runtime/state.h"
#include "plugins/extension_bag/game_ui_virtbag.h"

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

void publish_status_snapshot() {
    const auto& state = runtime_state();
    auto snapshot = std::make_shared<StatusSnapshot>();
    snapshot->bag = state.objects.bag;
    snapshot->enabled = state.enabled.load(std::memory_order_acquire);
    const int active_bags =
        virtual_bag::clamp_active_extension_bags(snapshot->bag.bag_count);
    for (int bag = 0; bag < active_bags; ++bag) {
        const int capacity = snapshot->bag.capacities[bag];
        for (int slot = 0; slot < capacity && slot < virtual_bag::kSlotCount; ++slot) {
            const virtual_bag::Item& item = snapshot->bag.items[bag][slot];
            if (item.category <= 0 || item.count <= 0) continue;
            snapshot->effective_counts[bag][slot] = virtual_bag_effective_count(
                state.objects.module_objects[bag][slot], static_cast<uint32_t>(item.count));
        }
    }
    snapshot->injected = state.persistence.state_entry != nullptr;
    snapshot->save_panel_hook_installed = state.persistence.save_panel_hook_installed;
    snapshot->extension_tab_button = state.ui.extension_tab_buttons[0] != nullptr;
    snapshot->inventory_frame_active = state.ui.inventory_frame_active;
    snapshot->recovery_action = virtual_bag::recovery_action(snapshot->bag, snapshot->bag.pending);
    snapshot->revision = state.persistence.revision.load(std::memory_order_acquire);

    constexpr uint32_t kCapacityMask = (1u << 25) - 1u;
    if (state.host.game != nullptr && state.host.write_port != nullptr) {
        for (int bag = 0; bag < static_cast<int>(snapshot->original_capacities.size()); ++bag) {
            uint32_t word = 0;
            if (bag_size_word_locked(bag, &word)) {
                snapshot->original_capacities[bag] = static_cast<int32_t>(word & kCapacityMask);
            }
        }
    }

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
