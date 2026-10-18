#include "plugins/extension_bag/extension_bag_internal.h"

#include "plugins/extension_bag/runtime/logging.h"

#include <chrono>
#include <cstring>

namespace {

constexpr uint64_t kP5SampleIntervalMs = 100;

}

bool p5_observation_enabled() {
    const auto* sdk = qol::features::extension_bag::extension_bag_host_sdk();
    return sdk != nullptr && sdk->log.debug_enabled != nullptr && sdk->log.debug_enabled() != 0;
}

bool p5_panel_observation_enabled() {
    return !g_p5_observation_active && p5_observation_enabled();
}

namespace {

uint64_t p5_steady_now_ms() {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}

bool p5_take_sample(std::atomic<uint64_t>* last_sample_ms) {
    const uint64_t now = p5_steady_now_ms();
    uint64_t previous = last_sample_ms->load(std::memory_order_relaxed);
    for (;;) {
        if (now >= previous && now - previous < kP5SampleIntervalMs) return false;
        if (last_sample_ms->compare_exchange_weak(previous, now, std::memory_order_relaxed,
                                                  std::memory_order_relaxed)) {
            return true;
        }
    }
}

}

bool p5_should_begin_item_observation(uint64_t event) {
    return event != 0x10 || p5_take_sample(&g_p5_last_item_query_sample_ms);
}

namespace {

bool p5_should_emit_observation(const char* phase, uint64_t event) {
    if (!p5_observation_enabled()) return false;
    if (g_p5_observation_active && std::strncmp(phase, "panel-", 6) == 0) return false;
    if (event != 0x19 || std::strncmp(phase, "panel-", 6) != 0) return true;
    if (std::strcmp(phase, "panel-entry") == 0) return false;
    return p5_take_sample(&g_p5_last_panel_move_sample_ms);
}

class P5ObservationScope {
public:
    P5ObservationScope() : was_active_(g_p5_observation_active) {
        g_p5_observation_active = true;
    }

    ~P5ObservationScope() {
        g_p5_observation_active = was_active_;
    }

private:
    bool was_active_;
};

enum class P5DataDomain : int {
    kUnavailable = -1,
    kNoData = 0,
    kEmpty = 1,
    kProjectedModuleItem = 2,
    kModuleTabItem = 3,
    kOriginalItem = 4,
    kUnknownItem = 5,
};

struct P5ControlObservation {
    int user_type = -1;
    int original_slot = -1;
    int projection_bag = -1;
    int projection_slot = -1;
    int tab_index = -1;
    P5DataDomain data_domain = P5DataDomain::kUnavailable;
    int parent_depth = -1;
    bool parent_reaches_projection_root = false;
};

struct P5TouchObservation {
    bool available = false;
    bool moving_present = false;
    bool drop_source_present = false;
    bool moving_matches_target = false;
    bool drop_source_matches_target = false;
    bool moving_matches_source = false;
    bool drop_source_matches_source = false;
    int64_t release_x = -1;
    int64_t release_y = -1;
};

P5ControlObservation p5_control_observation_locked(void* control) {
    P5ControlObservation observation;
    if (control == nullptr) return observation;

    observation.user_type = extension_bag_ui_user_type(control);
    observation.original_slot = extension_bag_ui_item_slot(control);
    observation.tab_index = extension_tab_index(control);

    void* item = extension_bag_ui_item(control);
    if (item == nullptr) {
        observation.data_domain = P5DataDomain::kEmpty;
    } else {
        int module_bag = -1;
        int module_slot = -1;
        if (module_slot_of_item_locked(item, &module_bag, &module_slot)) {
            observation.data_domain = P5DataDomain::kProjectedModuleItem;
        } else if (observation.tab_index >= 0 &&
                   g_tab_bag_items[observation.tab_index] == item) {
            observation.data_domain = P5DataDomain::kModuleTabItem;
        } else if (observation.original_slot >= 0 &&
                   observation.original_slot < virtual_bag::kSlotCount) {
            observation.data_domain = P5DataDomain::kOriginalItem;
        } else {
            observation.data_domain = P5DataDomain::kUnknownItem;
        }
    }

    constexpr int kP5ParentDepthLimit = 16;
    void* current = control;
    observation.parent_depth = 0;
    while (current != nullptr && observation.parent_depth < kP5ParentDepthLimit) {
        if (current == g_projected_item_root) {
            observation.parent_reaches_projection_root = true;
            break;
        }
        current = extension_bag_ui_parent(current);
        ++observation.parent_depth;
    }

    if (!g_module_view_installed || !virtual_bag::valid_index(g_module_view_index) ||
        g_projected_item_root == nullptr) {
        return observation;
    }
    int child_count = 0;
    if (!extension_bag_ui_child_count(g_projected_item_root, &child_count)) return observation;
    const int capacity = g_virtual_bag_state.capacities[g_module_view_index];
    const int limit = capacity < child_count ? capacity : child_count;
    for (int slot = 0; slot < limit; ++slot) {
        if (extension_bag_ui_child_at(g_projected_item_root, slot) == control) {
            observation.projection_bag = g_module_view_index;
            observation.projection_slot = slot;
            break;
        }
    }
    return observation;
}

P5TouchObservation p5_touch_observation_locked(void* target_control, void* source_control) {
    P5TouchObservation observation;
    void* moving = nullptr;
    void* drop_source = nullptr;
    if (!extension_bag_ui_touch_state(&moving, &drop_source, &observation.release_x,
                                      &observation.release_y)) return observation;
    observation.available = true;
    observation.moving_present = moving != nullptr;
    observation.drop_source_present = drop_source != nullptr;
    observation.moving_matches_target =
        target_control != nullptr && moving != nullptr && moving == target_control;
    observation.drop_source_matches_target =
        target_control != nullptr && drop_source != nullptr && drop_source == target_control;
    observation.moving_matches_source = source_control != nullptr && moving == source_control;
    observation.drop_source_matches_source =
        source_control != nullptr && drop_source == source_control;
    return observation;
}

}

uint64_t log_p5_observation_locked(const char* phase, void* target_control, uint64_t event,
                                   bool x2_present, bool param_present, void* source_control,
                                   bool result_known, uint64_t result, int64_t event_x,
                                   int64_t event_y, uint64_t related_sequence) {
    if (!p5_should_emit_observation(phase, event)) return 0;
    P5ObservationScope observation_scope;
    const P5ControlObservation target = p5_control_observation_locked(target_control);
    const P5ControlObservation source = p5_control_observation_locked(source_control);
    const P5TouchObservation touch = p5_touch_observation_locked(target_control, source_control);
    const uint64_t sequence = g_p5_observation_sequence.fetch_add(1) + 1;
    VIRTBAG_LOG(
        "p5obs seq=%llu related_seq=%llu phase=%s event=0x%llx x2=%d param=%d result_known=%d result=0x%llx event_x=%lld event_y=%lld target_type=%d target_slot=%d target_projection=%d/%d target_tab=%d target_data=%d target_parent_depth=%d target_parent_projection_root=%d source_known=%d source_type=%d source_slot=%d source_projection=%d/%d source_tab=%d source_data=%d source_parent_depth=%d source_parent_projection_root=%d touch=%d moving=%d drop_source=%d moving_target=%d drop_target=%d moving_source=%d drop_source_match=%d release_x=%lld release_y=%lld mode=%d selected=%d overlay=%d overlay_index=%d inventory_generation=%llu tab_generation=%llu capture=%d legacy_drag=%d",
        static_cast<unsigned long long>(sequence),
        static_cast<unsigned long long>(related_sequence), phase,
        static_cast<unsigned long long>(event), x2_present ? 1 : 0, param_present ? 1 : 0,
        result_known ? 1 : 0, static_cast<unsigned long long>(result),
        static_cast<long long>(event_x), static_cast<long long>(event_y), target.user_type,
        target.original_slot, target.projection_bag, target.projection_slot, target.tab_index,
        static_cast<int>(target.data_domain), target.parent_depth,
        target.parent_reaches_projection_root ? 1 : 0, source_control != nullptr ? 1 : 0,
        source.user_type, source.original_slot, source.projection_bag, source.projection_slot,
        source.tab_index, static_cast<int>(source.data_domain), source.parent_depth,
        source.parent_reaches_projection_root ? 1 : 0,
        touch.available ? 1 : 0, touch.moving_present ? 1 : 0,
        touch.drop_source_present ? 1 : 0, touch.moving_matches_target ? 1 : 0,
        touch.drop_source_matches_target ? 1 : 0, touch.moving_matches_source ? 1 : 0,
        touch.drop_source_matches_source ? 1 : 0,
        static_cast<long long>(touch.release_x), static_cast<long long>(touch.release_y),
        static_cast<int>(g_virtual_bag_state.mode), g_virtual_bag_state.selected,
        g_module_view_installed ? 1 : 0, g_module_view_index,
        static_cast<unsigned long long>(g_inventory_generation),
        static_cast<unsigned long long>(g_extension_tab_generation),
        g_extension_touch_capture ? 1 : 0, g_extension_drag.active ? 1 : 0);
    return sequence;
}
