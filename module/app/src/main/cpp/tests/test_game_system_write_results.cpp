#include <cstdio>
#include <cstring>
#include <string>

#include "../game_system.cpp"
#include "../game/adapter_ports/game_host_port.cpp"

#include "contracts/game_ports/ui_write_port.h"
#include "game/access/ui_state.h"

namespace {

int g_failures = 0;
int g_game_write_calls = 0;
int g_ui_action_calls = 0;
int g_ui_cancel_calls = 0;
uint64_t g_ui_last_cancel_id = 0;
int g_gamestate_reads = 0;
std::string g_player_json = "{\"player\":{\"version\":2}}";
std::string g_gamestate_json = "{\"screen\":\"world\",\"version\":2}";
int g_not_ready_gamestate_reads = 0;

void check(bool condition, const char* expression, int line) {
    if (condition) return;
    ++g_failures;
    std::printf("FAIL test_game_system_write_results.cpp:%d: %s\n", line, expression);
}

#define CHECK(condition) check((condition), #condition, __LINE__)

template <size_t Capacity>
std::string output_json(const char (&buffer)[Capacity], uint32_t size) {
    return std::string(buffer, size);
}

qol::game::SaveWriteResult save_result() {
    qol::game::SaveWriteResult result{};
    constexpr char kSuccess[] = "{\"ok\":true}";
    std::memcpy(result.text, kSuccess, sizeof(kSuccess) - 1);
    result.size = sizeof(kSuccess) - 1;
    return result;
}

qol::game::WorldWriteResult world_result() {
    qol::game::WorldWriteResult result{};
    constexpr char kSuccess[] = "{\"ok\":true}";
    std::memcpy(result.text, kSuccess, sizeof(kSuccess) - 1);
    result.size = sizeof(kSuccess) - 1;
    return result;
}

qol::game::UiWriteResult ui_result() {
    qol::game::UiWriteResult result{};
    constexpr char kSuccess[] = "{\"ok\":true}";
    std::memcpy(result.text, kSuccess, sizeof(kSuccess) - 1);
    result.size = sizeof(kSuccess) - 1;
    return result;
}

}  // namespace

std::string data_player_json() { return g_player_json; }
bool game_in_world() { return true; }
std::string data_gamestate_json() {
    ++g_gamestate_reads;
    if (g_not_ready_gamestate_reads > 0) {
        --g_not_ready_gamestate_reads;
        return "{\"error\":\"not ready\"}";
    }
    return g_gamestate_json;
}
void frame_cache_force_refresh() {}

std::string op_ok() {
    ++g_game_write_calls;
    return "{\"ok\":true}";
}
std::string op_err(const char* message) {
    return std::string("{\"ok\":false,\"error\":\"") + message + "\"}";
}

namespace qol::game {

QuestQuitResult quest_quit(int32_t) {
    ++g_game_write_calls;
    return QuestQuitResult{true, nullptr};
}

SaveWriteResult enter_slot_direct(int32_t) {
    ++g_game_write_calls;
    return save_result();
}
SaveWriteResult create_slot_direct(int32_t, int32_t) {
    ++g_game_write_calls;
    return save_result();
}
WorldWriteResult world_move(int32_t, int32_t) {
    ++g_game_write_calls;
    return world_result();
}
WorldWriteResult world_walk(int32_t) {
    ++g_game_write_calls;
    return world_result();
}
WorldWriteResult world_walk_stop() {
    ++g_game_write_calls;
    return world_result();
}
WorldWriteResult world_interact() {
    ++g_game_write_calls;
    return world_result();
}
WorldWriteResult world_teleport_direct(int32_t, int32_t, int32_t, bool) {
    ++g_game_write_calls;
    return world_result();
}

UiWriteResult ui_panel_open(const std::string&) {
    ++g_game_write_calls;
    return ui_result();
}
UiWriteResult ui_panel_close() {
    ++g_game_write_calls;
    return ui_result();
}
UiWriteResult ui_main_menu(bool) {
    ++g_game_write_calls;
    return ui_result();
}

QolResult ui_state_write_step(uint64_t, uint32_t, int32_t, const char*, char* out,
                              uint32_t capacity, uint32_t* out_size) {
    ++g_ui_action_calls;
    constexpr char kSuccess[] = "{\"ok\":true}";
    if (out == nullptr || out_size == nullptr || capacity < sizeof(kSuccess) - 1) {
        return contracts::error(kQolErrInternal, "UI result buffer unavailable", 28);
    }
    std::memcpy(out, kSuccess, sizeof(kSuccess) - 1);
    *out_size = sizeof(kSuccess) - 1;
    return contracts::ok();
}

QolResult ui_state_write_cancel_pending(uint64_t operation_id) {
    ++g_ui_cancel_calls;
    g_ui_last_cancel_id = operation_id;
    return contracts::ok();
}

}  // namespace qol::game

int main() {
    const std::string expected_success = "{\"ok\":true,\"state\":{\"player\":{\"version\":2}}}";
    const std::string expected_not_ready = "{\"ok\":false,\"error\":\"not ready\"}";

    CHECK(qol::game::build_quest_quit_result_json(7) == expected_success);
    g_player_json = "{\"error\":\"not ready\"}";
    CHECK(qol::game::build_quest_quit_result_json(7) == expected_not_ready);
    g_player_json = "{\"player\":{\"version\":2}}";

    char output[1024]{};
    uint32_t output_size = 0;
    int32_t outcome = qol::game::build_save_write_result_json(
        qol::contracts::kQolSaveWriteEnterSlot, 0, 0, 0, output, sizeof(output), &output_size);
    CHECK(outcome == qol::game::kSaveWriteOk);
    CHECK(output_json(output, output_size) == expected_success);
    g_player_json = "{\"error\":\"not ready\"}";
    const int writes_before_save_error = g_game_write_calls;
    outcome = qol::game::build_save_write_result_json(
        qol::contracts::kQolSaveWriteEnterSlot, 0, 0, 0, output, sizeof(output), &output_size);
    CHECK(outcome == qol::game::kSaveWriteFailed);
    CHECK(output_json(output, output_size) == expected_not_ready);
    CHECK(g_game_write_calls == writes_before_save_error + 1);

    g_player_json = "{\"player\":{\"version\":2}}";
    outcome = qol::game::build_world_write_result_json(qol::contracts::kQolWorldWriteMoveTo, 1,
                                                       2, 0, 0, output, sizeof(output),
                                                       &output_size);
    CHECK(outcome == qol::game::kWorldWriteOk);
    CHECK(output_json(output, output_size) == expected_success);
    g_player_json = "{\"error\":\"not ready\"}";
    outcome = qol::game::build_world_write_result_json(qol::contracts::kQolWorldWriteMoveTo, 1,
                                                       2, 0, 0, output, sizeof(output),
                                                       &output_size);
    CHECK(outcome == qol::game::kWorldWriteFailed);
    CHECK(output_json(output, output_size) == expected_not_ready);

    g_gamestate_json = "{\"screen\":\"world\",\"version\":2}";
    outcome = qol::game::build_ui_write_result_json(qol::contracts::kQolUiWritePanelOpen,
                                                    "inventory", output, sizeof(output),
                                                    &output_size);
    CHECK(outcome == qol::game::kUiWriteOk);
    CHECK(output_json(output, output_size) ==
          "{\"ok\":true,\"state\":{\"screen\":\"world\",\"version\":2}}");
    g_not_ready_gamestate_reads = 1;
    outcome = qol::game::build_ui_write_result_json(qol::contracts::kQolUiWritePanelOpen,
                                                    "inventory", output, sizeof(output),
                                                    &output_size);
    CHECK(outcome == qol::game::kUiWriteFailed);
    CHECK(output_json(output, output_size) == expected_not_ready);

    // The real GameHost port retries only state assembly after the one-time UI action applied.
    qol::game::game_host_mark_main_thread();
    const auto& ui_port = qol::game::ui_write_port();
    char ui_output[512]{};
    uint32_t ui_output_size = 0;
    const int actions_before_pending = g_ui_action_calls;
    g_not_ready_gamestate_reads = 1;
    QolResult ui_result = ui_port.read.step(90, 0, qol::contracts::kQolUiWritePanelOpen,
                                            "inventory", ui_output, sizeof(ui_output),
                                            &ui_output_size);
    CHECK(ui_result.code == kQolErrNotReady && ui_output_size == 0);
    CHECK(g_ui_action_calls == actions_before_pending + 1);
    g_gamestate_json = "{\"screen\":\"world\",\"version\":3}";
    ui_result = ui_port.read.step(90, 1, qol::contracts::kQolUiWritePanelOpen, "inventory",
                                  ui_output, sizeof(ui_output), &ui_output_size);
    CHECK(qol::contracts::succeeded(ui_result));
    CHECK(std::string(ui_output, ui_output_size) ==
          "{\"ok\":true,\"state\":{\"screen\":\"world\",\"version\":3}}");
    CHECK(g_ui_action_calls == actions_before_pending + 1);

    const int actions_before_cancel = g_ui_action_calls;
    g_not_ready_gamestate_reads = 1;
    ui_result = ui_port.read.step(91, 0, qol::contracts::kQolUiWritePanelOpen, "inventory",
                                  ui_output, sizeof(ui_output), &ui_output_size);
    CHECK(ui_result.code == kQolErrNotReady);
    CHECK(qol::contracts::succeeded(ui_port.read.cancel_pending(91)));
    CHECK(g_ui_cancel_calls == 1);
    CHECK(g_ui_last_cancel_id == 91);
    CHECK(g_ui_action_calls == actions_before_cancel + 1);

    // A lifecycle cancel with id 0 clears the applied operation's pending result snapshot.
    const int actions_before_zero_cancel = g_ui_action_calls;
    g_not_ready_gamestate_reads = 1;
    ui_result = ui_port.read.step(92, 0, qol::contracts::kQolUiWritePanelOpen, "inventory",
                                  ui_output, sizeof(ui_output), &ui_output_size);
    CHECK(ui_result.code == kQolErrNotReady);
    CHECK(g_ui_action_calls == actions_before_zero_cancel + 1);

    const int cancel_calls_before_mismatch = g_ui_cancel_calls;
    CHECK(qol::contracts::succeeded(ui_port.read.cancel_pending(999)));
    CHECK(g_ui_cancel_calls == cancel_calls_before_mismatch + 1);
    CHECK(g_ui_last_cancel_id == 999);
    ui_result = ui_port.read.step(93, 0, qol::contracts::kQolUiWritePanelOpen, "inventory",
                                  ui_output, sizeof(ui_output), &ui_output_size);
    CHECK(ui_result.code == kQolErrNotReady);
    CHECK(g_ui_action_calls == actions_before_zero_cancel + 1);

    CHECK(qol::contracts::succeeded(ui_port.read.cancel_pending(0)));
    CHECK(g_ui_last_cancel_id == 0);
    const int actions_before_new_operation = g_ui_action_calls;
    g_gamestate_json = "{\"screen\":\"world\",\"version\":4}";
    ui_result = ui_port.read.step(93, 0, qol::contracts::kQolUiWritePanelOpen, "inventory",
                                  ui_output, sizeof(ui_output), &ui_output_size);
    CHECK(qol::contracts::succeeded(ui_result));
    CHECK(std::string(ui_output, ui_output_size) ==
          "{\"ok\":true,\"state\":{\"screen\":\"world\",\"version\":4}}");
    CHECK(g_ui_action_calls == actions_before_new_operation + 1);

    if (g_failures != 0) {
        std::printf("game_system_write_results: %d failure(s)\n", g_failures);
        return 1;
    }
    std::puts("game_system_write_results: pass");
    return 0;
}
