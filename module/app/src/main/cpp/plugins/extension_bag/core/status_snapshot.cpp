#include "plugins/extension_bag/core/ui_state.h"
#include "plugins/extension_bag/runtime/bag_state_store.h"
#include "plugins/extension_bag/runtime/item_access.h"
#include "plugins/extension_bag/runtime/projection_controller.h"
#include "plugins/extension_bag/runtime/state.h"
#include "plugins/extension_bag/runtime/status_snapshot.h"

namespace qol::extension_bag {

namespace {

BagProjectionSnapshot current_projection_snapshot() {
    const auto& state = ui_state();
    return {state.objects.module_view_installed, state.objects.module_view_index,
            state.ui.inventory_generation};
}

}  // namespace

void publish_extension_bag_snapshot() {
    publish_bag_snapshot(current_projection_snapshot());
}

void publish_extension_bag_snapshot_if_changed() {
    publish_bag_snapshot_if_changed(current_projection_snapshot());
}

void publish_extension_bag_status_snapshot() {
    const auto& state = runtime_state();
    const auto& ui = ui_state();
    StatusSnapshot snapshot{};
    snapshot.bag = state.objects.bag;
    snapshot.enabled = state.enabled.load(std::memory_order_acquire);
    const int active_bags = virtual_bag::clamp_active_extension_bags(snapshot.bag.bag_count);
    for (int bag = 0; bag < active_bags; ++bag) {
        const int capacity = snapshot.bag.capacities[bag];
        for (int slot = 0; slot < capacity && slot < virtual_bag::kSlotCount; ++slot) {
            const virtual_bag::Item& item = snapshot.bag.items[bag][slot];
            if (item.category <= 0 || item.count <= 0) continue;
            snapshot.effective_counts[bag][slot] = virtual_bag_effective_count(
                ui.objects.module_objects[bag][slot], static_cast<uint32_t>(item.count));
        }
    }
    snapshot.injected = state.persistence.state_entry != nullptr;
    snapshot.save_panel_hook_installed = ui.persistence_hooks.save_panel_hook_installed;
    snapshot.extension_tab_button = ui.ui.extension_tab_buttons[0] != nullptr;
    snapshot.inventory_frame_active = ui.ui.inventory_frame_active;
    snapshot.recovery_action = virtual_bag::recovery_action(snapshot.bag, snapshot.bag.pending);
    snapshot.revision = state.persistence.revision.load(std::memory_order_acquire);

    constexpr uint32_t kCapacityMask = (1u << 25) - 1u;
    if (state.host.game != nullptr && state.host.write_port != nullptr) {
        for (int bag = 0; bag < static_cast<int>(snapshot.original_capacities.size()); ++bag) {
            uint32_t word = 0;
            if (bag_size_word_locked(bag, &word)) {
                snapshot.original_capacities[bag] = static_cast<int32_t>(word & kCapacityMask);
            }
        }
    }
    publish_status_snapshot(snapshot);
}

}  // namespace qol::extension_bag
