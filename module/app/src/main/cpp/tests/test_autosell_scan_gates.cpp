#include "plugins/autosell/scan_gates.h"

#include <cstdio>
#include <cstring>

namespace {

int g_pass = 0;
int g_fail = 0;
#define CHECK(condition) do { \
    if (condition) ++g_pass; \
    else { ++g_fail; std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition); } \
} while (false)

bool g_world_available = true;
int32_t g_world_state = 5;
bool g_game_available = true;
int32_t g_game_gate = 1;
bool g_panel_query_available = true;
int32_t g_panel_is_active = 0;
bool g_ui_is_busy = false;
char g_trace[16]{};
size_t g_trace_size = 0;

void trace(char value) {
    if (g_trace_size + 1 < sizeof(g_trace)) {
        g_trace[g_trace_size++] = value;
        g_trace[g_trace_size] = '\0';
    }
}

void reset() {
    g_world_available = true;
    g_world_state = 5;
    g_game_available = true;
    g_game_gate = 1;
    g_panel_query_available = true;
    g_panel_is_active = 0;
    g_ui_is_busy = false;
    g_trace[0] = '\0';
    g_trace_size = 0;
}

bool world_state(int32_t* out) {
    trace('w');
    if (out == nullptr || !g_world_available) return false;
    *out = g_world_state;
    return true;
}

bool game_state_gate(int32_t* out) {
    trace('g');
    if (out == nullptr || !g_game_available) return false;
    *out = g_game_gate;
    return true;
}

bool panel_active(int32_t* out) {
    trace('p');
    if (out == nullptr || !g_panel_query_available) return false;
    *out = g_panel_is_active;
    return true;
}

bool ui_busy() {
    trace('u');
    return g_ui_is_busy;
}

const ScanGateDeps kDeps{&world_state, &game_state_gate, &panel_active, &ui_busy};

void test_gate_reasons_follow_order() {
    ScanGateReason reason = ScanGateReason::kReady;

    reset();
    g_world_available = false;
    CHECK(!autosell_scan_gates_ok(kDeps, &reason));
    CHECK(reason == ScanGateReason::kWorldUnavailable);
    CHECK(std::strcmp(g_trace, "w") == 0);

    reset();
    g_world_state = 4;
    CHECK(!autosell_scan_gates_ok(kDeps, &reason));
    CHECK(reason == ScanGateReason::kNotInWorld);
    CHECK(std::strcmp(g_trace, "w") == 0);

    reset();
    g_game_available = false;
    CHECK(!autosell_scan_gates_ok(kDeps, &reason));
    CHECK(reason == ScanGateReason::kGameStateUnavailable);
    CHECK(std::strcmp(g_trace, "wg") == 0);

    reset();
    g_game_gate = 0;
    CHECK(!autosell_scan_gates_ok(kDeps, &reason));
    CHECK(reason == ScanGateReason::kGameStateBlocked);
    CHECK(std::strcmp(g_trace, "wg") == 0);

    reset();
    g_panel_is_active = 1;
    CHECK(!autosell_scan_gates_ok(kDeps, &reason));
    CHECK(reason == ScanGateReason::kUiBusy);
    CHECK(std::strcmp(g_trace, "wgp") == 0);

    reset();
    g_ui_is_busy = true;
    CHECK(!autosell_scan_gates_ok(kDeps, &reason));
    CHECK(reason == ScanGateReason::kUiBusy);
    CHECK(std::strcmp(g_trace, "wgpu") == 0);
}

void test_ready_path_and_soft_panel_query_failure() {
    ScanGateReason reason = ScanGateReason::kWorldUnavailable;
    reset();
    CHECK(autosell_scan_gates_ok(kDeps, &reason));
    CHECK(reason == ScanGateReason::kReady);
    CHECK(std::strcmp(g_trace, "wgpu") == 0);

    reset();
    g_panel_query_available = false;
    CHECK(autosell_scan_gates_ok(kDeps, &reason));
    CHECK(reason == ScanGateReason::kReady);
    CHECK(std::strcmp(g_trace, "wgpu") == 0);
}

void test_reason_names_preserve_enum_contract() {
    CHECK(std::strcmp(autosell_scan_gate_name(ScanGateReason::kReady), "ready") == 0);
    CHECK(std::strcmp(autosell_scan_gate_name(ScanGateReason::kGlobalDisabled), "global_disabled") == 0);
    CHECK(std::strcmp(autosell_scan_gate_name(ScanGateReason::kSessionInactive), "session_inactive") == 0);
    CHECK(std::strcmp(autosell_scan_gate_name(ScanGateReason::kConfigDisabled), "config_disabled") == 0);
    CHECK(std::strcmp(autosell_scan_gate_name(ScanGateReason::kWorldUnavailable), "world_unavailable") == 0);
    CHECK(std::strcmp(autosell_scan_gate_name(ScanGateReason::kNotInWorld), "not_in_world") == 0);
    CHECK(std::strcmp(autosell_scan_gate_name(ScanGateReason::kGameStateUnavailable), "game_state_unavailable") == 0);
    CHECK(std::strcmp(autosell_scan_gate_name(ScanGateReason::kGameStateBlocked), "game_state_blocked") == 0);
    CHECK(std::strcmp(autosell_scan_gate_name(ScanGateReason::kUiBusy), "ui_busy") == 0);
}

}  // namespace

int main() {
    test_gate_reasons_follow_order();
    test_ready_path_and_soft_panel_query_failure();
    test_reason_names_preserve_enum_contract();
    std::printf("autosell_scan_gates_tests: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
