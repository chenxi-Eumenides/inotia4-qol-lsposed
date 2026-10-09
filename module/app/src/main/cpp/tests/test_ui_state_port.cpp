#include "game/adapter_ports/ui_write_port.h"

#include <cassert>
#include <cstring>
#include <string>

#include "game/state/game_state.h"
#include "game/state/ui_http.h"

namespace qol::game {
const contracts::QolUiStateReadPort& ui_state_read_port();
}

namespace {

bool g_main_thread = true;
const char* g_screen = "world";
std::string g_dialog = "{\"type\":\"none\",\"options\":[]}";

}  // namespace

namespace qol::game {

bool game_host_is_main_thread() { return g_main_thread; }
std::string dialog_content_json() { return g_dialog; }
const char* top_panel_name() { return nullptr; }
const char* popup_stack_snapshot_failure() { return nullptr; }
bool popup_stack_snapshot(uint32_t*, uintptr_t*) { return false; }
uintptr_t popup_top_vma() { return 0; }

int32_t build_ui_write_result_json(int32_t, const char*, char*, uint32_t,
                                  uint32_t* out_size) {
    if (out_size != nullptr) *out_size = 0;
    return kUiWriteOk;
}

}  // namespace qol::game

const char* ui_screen() { return g_screen; }

int main() {
    const auto& port = qol::game::ui_state_read_port();
    char screen[64]{};
    char panel[128]{};
    char dialog[256]{};
    uint32_t screen_size = 0;
    uint32_t panel_size = 0;
    uint32_t dialog_size = 0;
    assert(port.read.read_snapshot(screen, sizeof(screen), &screen_size, panel, sizeof(panel),
                                   &panel_size, dialog, sizeof(dialog), &dialog_size).ok == kQolOk);
    assert(std::strcmp(screen, "world") == 0 && screen_size == 5);
    assert(panel[0] == '\0' && panel_size == 0);
    assert(std::strcmp(dialog, g_dialog.c_str()) == 0);

    g_screen = "panel_inventory";
    g_dialog = "{\"type\":\"inventory\",\"options\":[]}";
    assert(port.read.read_snapshot(screen, sizeof(screen), &screen_size, panel, sizeof(panel),
                                   &panel_size, dialog, sizeof(dialog), &dialog_size).ok == kQolOk);
    assert(std::strcmp(panel, "panel_inventory") == 0);
    assert(std::strcmp(dialog, g_dialog.c_str()) == 0);

    g_main_thread = false;
    assert(port.read.read_snapshot(screen, sizeof(screen), &screen_size, panel, sizeof(panel),
                                   &panel_size, dialog, sizeof(dialog), &dialog_size).code ==
           kQolErrBlocked);
    g_main_thread = true;
    assert(port.read.read_snapshot(screen, 1, &screen_size, panel, sizeof(panel), &panel_size,
                                   dialog, sizeof(dialog), &dialog_size).code == kQolErrNotReady);
    return 0;
}
