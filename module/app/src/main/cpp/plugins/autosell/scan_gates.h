#pragma once

#include <cstdint>

enum class ScanGateReason : int {
    kReady,
    kGlobalDisabled,
    kSessionInactive,
    kConfigDisabled,
    kWorldUnavailable,
    kNotInWorld,
    kGameStateUnavailable,
    kGameStateBlocked,
    kUiBusy,
};

struct ScanGateDeps {
    bool (*world_state)(int32_t* out);
    bool (*game_state_gate)(int32_t* out);
    bool (*panel_active)(int32_t* out);
    bool (*ui_busy)();
};

const char* autosell_scan_gate_name(ScanGateReason reason);
bool autosell_scan_gates_ok(const ScanGateDeps& deps, ScanGateReason* out_reason);
