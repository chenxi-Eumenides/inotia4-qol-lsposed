#include "plugins/extension_bag/runtime/bag_state_store.h"

#include "plugins/extension_bag/runtime/state.h"
#include "plugins/extension_bag/runtime/status_snapshot.h"

#include <memory>

namespace qol::extension_bag {
namespace {

std::shared_ptr<const BagSnapshot> g_snapshot;

}  // namespace

void publish_bag_snapshot() {
    auto snapshot = std::make_shared<BagSnapshot>();
    const auto& bag = runtime_state().objects.bag;
    snapshot->types = bag.types;
    snapshot->capacities = bag.capacities;
    snapshot->mode = static_cast<int32_t>(bag.mode);
    snapshot->selected = bag.selected;
    snapshot->inspected = bag.inspected;
    snapshot->module_view_installed = runtime_state().objects.module_view_installed;
    snapshot->module_view_index = runtime_state().objects.module_view_index;
    snapshot->revision = runtime_state().persistence.revision.load(std::memory_order_acquire);
    snapshot->inventory_generation = runtime_state().ui.inventory_generation;
    std::atomic_store_explicit(&g_snapshot, std::shared_ptr<const BagSnapshot>(snapshot),
                               std::memory_order_release);
    publish_status_snapshot();
}

void publish_bag_snapshot_if_changed() {
    const uint64_t revision =
        runtime_state().persistence.revision.load(std::memory_order_acquire);
    static uint64_t published_revision = 0;
    static bool published_once = false;
    if (published_once && published_revision == revision) return;
    published_revision = revision;
    published_once = true;
    publish_bag_snapshot();
}

bool bag_snapshot(BagSnapshot* out) {
    if (out == nullptr) return false;
    const auto snapshot = std::atomic_load_explicit(&g_snapshot, std::memory_order_acquire);
    if (!snapshot) return false;
    *out = *snapshot;
    return true;
}

}  // namespace qol::extension_bag
