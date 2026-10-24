#include "plugins/extension_bag/interface/bag_query.h"

#include "plugins/extension_bag/extension_bag_context.h"
#include "plugins/extension_bag/game_ui_virtbag.h"
#include "plugins/extension_bag/model/virtual_bag_state.h"
#include "plugins/extension_bag/runtime/bag_state_store.h"
#include "plugins/extension_bag/runtime/inventory_access.h"
#include "plugins/extension_bag/runtime/logging.h"
#include "plugins/extension_bag/runtime/status_snapshot.h"

#include <atomic>

namespace qol::features::extension_bag {

std::string query_status_json() {
    const auto snapshot = qol::extension_bag::load_status_snapshot();
    if (snapshot) return qol::extension_bag::status_snapshot_json(*snapshot);

    static std::atomic_flag not_ready_logged = ATOMIC_FLAG_INIT;
    if (!not_ready_logged.test_and_set(std::memory_order_relaxed)) {
        QOL_LOG_WARN(QolDomain::kExtensionBag,
                     "status snapshot unavailable; returning not-ready status");
    }
    return "{\"enabled\":" + std::string(virtual_bag_enabled() ? "true" : "false") +
           ",\"injected\":false,\"extension_tab_button\":false," +
           "\"inventory_frame_active\":false,\"recovery_action\":\"none\",\"state\":{}}";
}

bool query_bag_capacity(int logical_bag, int* out_capacity) {
    if (out_capacity == nullptr) return false;
    *out_capacity = 0;
    if (!virtual_bag::valid_extension_logical_bag(logical_bag) ||
        !virtual_bag_enabled() || !read_world_in_world()) {
        return false;
    }
    if (!extension_bag_session_admitted_locked()) return false;

    const int internal_bag = virtual_bag::extension_internal_bag(logical_bag);
    if (!virtual_bag::valid_index(internal_bag)) return false;
    qol::extension_bag::BagSnapshot snapshot{};
    if (!qol::extension_bag::bag_snapshot(&snapshot)) return false;
    *out_capacity = snapshot.capacities[internal_bag];
    return true;
}

std::string query_inventory_fragment() {
    return virtual_bag_inventory_bags_json();
}

}  // namespace qol::features::extension_bag
