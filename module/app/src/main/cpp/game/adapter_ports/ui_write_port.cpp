#include "game/adapter_ports/ui_write_port.h"

#include "contracts/game_ports/ports.h"
#include "game/access/ui_state.h"
#include "game/adapter_ports/game_host_port.h"
#include "game/state/game_state.h"
#include "game/state/ui_http.h"

#include <cstring>
#include <string>

namespace qol::game {
namespace {

QolResult write_adapter(int32_t kind, const char* panel, char* out, uint32_t capacity,
                        uint32_t* out_size) {
    const int32_t outcome = build_ui_write_result_json(kind, panel, out, capacity, out_size);
    if (outcome == kUiWriteFailed) {
        // 业务失败时 out 已携带原错误体；返回错误让结果处理器把票据置 failed（结果仍可回取）。
        return contracts::error(kQolErrInternal, "ui write failed", 15);
    }
    return contracts::ok();
}

bool is_panel_screen(const char* screen) {
    return screen != nullptr &&
           (std::strncmp(screen, "panel_", 6) == 0 ||
            std::strncmp(screen, "main_menu_", 10) == 0);
}

QolResult copy_snapshot_field(const std::string& value, char* out, uint32_t capacity,
                              uint32_t* out_size) {
    if (out == nullptr || out_size == nullptr || capacity == 0) {
        return contracts::error(kQolErrInvalidArgument, "invalid UI snapshot output", 26);
    }
    if (value.size() >= capacity) return contracts::error(kQolErrNotReady, "UI snapshot too large", 21);
    if (!value.empty()) std::memcpy(out, value.data(), value.size());
    out[value.size()] = '\0';
    *out_size = static_cast<uint32_t>(value.size());
    return contracts::ok();
}

QolResult read_ui_state_snapshot(char* screen, uint32_t screen_capacity, uint32_t* screen_size,
                                char* panel, uint32_t panel_capacity, uint32_t* panel_size,
                                char* dialog, uint32_t dialog_capacity, uint32_t* dialog_size) {
    if (screen == nullptr || screen_size == nullptr || panel == nullptr || panel_size == nullptr ||
        dialog == nullptr || dialog_size == nullptr || screen_capacity == 0 ||
        panel_capacity == 0 || dialog_capacity == 0) {
        return contracts::error(kQolErrInvalidArgument, "invalid UI snapshot output", 25);
    }
    if (!game_host_is_main_thread()) {
        return contracts::error(kQolErrBlocked, "wrong thread", 12);
    }
    const char* native_screen = ::ui_screen();
    if (native_screen == nullptr) return contracts::error(kQolErrNotReady, "screen unavailable", 18);
    const char* native_panel = is_panel_screen(native_screen) ? native_screen : nullptr;
    const std::string native_dialog = dialog_content_json();
    const QolResult screen_result =
        copy_snapshot_field(native_screen, screen, screen_capacity, screen_size);
    if (!contracts::succeeded(screen_result)) return screen_result;
    const QolResult panel_result =
        copy_snapshot_field(native_panel == nullptr ? std::string() : std::string(native_panel),
                            panel, panel_capacity, panel_size);
    if (!contracts::succeeded(panel_result)) return panel_result;
    return copy_snapshot_field(native_dialog, dialog, dialog_capacity, dialog_size);
}

const contracts::QolUiWritePort kPort{contracts::kQolPortUiWriteVersion, {&write_adapter}};
const contracts::QolUiStateReadPort kStateReadPort{
    contracts::kQolPortUiStateReadVersion, {&read_ui_state_snapshot}};

}  // namespace

const contracts::QolUiWritePort& ui_write_port() { return kPort; }
const contracts::QolUiStateReadPort& ui_state_read_port() { return kStateReadPort; }

}  // namespace qol::game
