#include "game/access/ui_state.h"
#include "game/access/game_access.h"
#include "game/access/save.h"
#include "game/ops/game_ops_common.h"
#include "game/symbols/game_symbols.h"
#include "contracts/extension_bag_port.h"
#include "contracts/popup_enter_alias.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <sys/mman.h>

uintptr_t g_base = 0;
void* g_popup_stack = nullptr;
void* g_popup_on = nullptr;
IntVoidFn fn_wipeout_button_gameover = nullptr;
IntVoidFn fn_event_button_ok_exe = nullptr;
IntVoidFn fn_event_button_skip_exe = nullptr;
UinpcQuestButtonOkExeFn fn_uinpc_quest_button_ok_exe = nullptr;
UiSetPopupProcessInfoFn fn_ui_set_popup_process_info = nullptr;
UinpcExeTaskFn fn_uinpc_exe_current_task = nullptr;

namespace {
int g_pass = 0;
int g_fail = 0;
int g_game_over_calls = 0;
int g_in_world = 1;
int g_story = 0;

#define CHECK(cond) do { \
    if (cond) ++g_pass; \
    else { ++g_fail; std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } \
} while (0)

int game_over() { ++g_game_over_calls; return 0; }
}

bool game_in_world() { return g_in_world != 0; }

namespace qol::game {
bool story_active() { return g_story != 0; }
SaveWriteResult save_game() { return {}; }
}

std::string op_err(const char* msg) { return std::string("{\"ok\":false,\"error\":\"") + msg + "\"}"; }
std::string op_ok() { return "{\"ok\":true}"; }
std::string json_escape(const char* text) { return text == nullptr ? "" : text; }

const qol::contracts::ExtensionBagApi* extension_bag_capability() { return nullptr; }
uintptr_t popup_enter_alias_resolve(uintptr_t) { return kPopupEnterAliasNone; }

int main() {
    using namespace qol::game;
    auto* base = static_cast<uint8_t*>(mmap(nullptr, 0x400000, PROT_READ | PROT_WRITE | PROT_EXEC,
                                            MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
    CHECK(base != MAP_FAILED);
    if (base == MAP_FAILED) return 1;

    g_base = reinterpret_cast<uintptr_t>(base);
    base[F_BUTTON_OK_EXE_VMA] = 0xc3;
    base[F_BUTTON_CANCEL_EXE_VMA] = 0xc3;
    base[G_POPUP_ON_VMA] = 1;
    uint8_t popup_on = 1;
    g_popup_on = &popup_on;

    struct FakePopupStack {
        uint64_t reserved = 0;
        uint32_t count = 1;
        uint32_t padding = 0;
        uint64_t reserved2 = 0;
        uint64_t data = 0;
    } stack;
    struct FakePopupEntry {
        uint8_t padding[POPUP_ENTRY_ENTER]{};
        uintptr_t enter = 0;
    } entry;
    stack.data = reinterpret_cast<uintptr_t>(&entry);
    g_popup_stack = &stack;
    fn_wipeout_button_gameover = &game_over;

    entry.enter = g_base + F_PANEL_WIPEOUT_ENTER;
    CHECK(dialog_select("ok", -1).str() == "{\"ok\":false,\"error\":\"no such option in wipeout\"}");
    CHECK(dialog_select("cancel", -1).str() == "{\"ok\":false,\"error\":\"no such option in wipeout\"}");
    CHECK(dialog_ok().str() == "{\"ok\":false,\"error\":\"no such option in wipeout\"}");
    CHECK(dialog_cancel().str() == "{\"ok\":false,\"error\":\"no such option in wipeout\"}");
    CHECK(g_game_over_calls == 0);
    CHECK(dialog_select("game_over", -1).str() == "{\"ok\":true}");
    CHECK(g_game_over_calls == 1);

    entry.enter = g_base + F_PANEL_CHOICE_ENTER;
    CHECK(dialog_select("ok", -1).str() == "{\"ok\":true}");

    munmap(base, 0x400000);
    std::printf("ui_state_dialog: pass=%d fail=%d\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
