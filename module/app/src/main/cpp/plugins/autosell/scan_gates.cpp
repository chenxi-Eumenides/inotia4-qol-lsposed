#include "scan_gates.h"

const char* autosell_scan_gate_name(ScanGateReason reason) {
    switch (reason) {
        case ScanGateReason::kReady: return "ready";
        case ScanGateReason::kGlobalDisabled: return "global_disabled";
        case ScanGateReason::kSessionInactive: return "session_inactive";
        case ScanGateReason::kConfigDisabled: return "config_disabled";
        case ScanGateReason::kWorldUnavailable: return "world_unavailable";
        case ScanGateReason::kNotInWorld: return "not_in_world";
        case ScanGateReason::kGameStateUnavailable: return "game_state_unavailable";
        case ScanGateReason::kGameStateBlocked: return "game_state_blocked";
        case ScanGateReason::kUiBusy: return "ui_busy";
    }
    return "unknown";
}

bool autosell_scan_gates_ok(const ScanGateDeps& deps, ScanGateReason* out_reason) {
    if (out_reason != nullptr) *out_reason = ScanGateReason::kReady;

    int32_t world_state = -1;
    if (deps.world_state == nullptr || !deps.world_state(&world_state)) {
        if (out_reason != nullptr) *out_reason = ScanGateReason::kWorldUnavailable;
        return false;
    }
    if (world_state != 5) {
        if (out_reason != nullptr) *out_reason = ScanGateReason::kNotInWorld;
        return false;
    }

    int32_t state_gate = 0;
    if (deps.game_state_gate == nullptr || !deps.game_state_gate(&state_gate)) {
        if (out_reason != nullptr) *out_reason = ScanGateReason::kGameStateUnavailable;
        return false;
    }
    if (state_gate == 0) {
        if (out_reason != nullptr) *out_reason = ScanGateReason::kGameStateBlocked;
        return false;
    }

    int32_t panel_active = 0;
    if (deps.panel_active != nullptr && deps.panel_active(&panel_active) && panel_active != 0) {
        if (out_reason != nullptr) *out_reason = ScanGateReason::kUiBusy;
        return false;
    }
    if (deps.ui_busy == nullptr || deps.ui_busy()) {
        if (out_reason != nullptr) *out_reason = ScanGateReason::kUiBusy;
        return false;
    }
    return true;
}
