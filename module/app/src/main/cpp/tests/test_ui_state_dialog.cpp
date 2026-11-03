#include "game/access/ui_state.h"
#include "game/access/game_access.h"
#include "game/access/save.h"
#include "game/ops/game_ops_common.h"
#include "game/symbols/game_symbols.h"
#include "contracts/popup_enter_alias.h"
#include "contracts/lifecycle_participant.h"
#include "contracts/runtime_hooks.h"
#include "contracts/game_ports/ui_write_port.h"
#include "game/adapter_ports/lifecycle_participant_facade.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
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
GamestateSetStateFn fn_gamestate_set_state = nullptr;
GetNameFn fn_get_name = nullptr;

namespace {

constexpr size_t kFakeGameSize = 0x800000;

}  // namespace

// 仅认可测试 mmap 出的假 libgame 区间可读，其余地址（含悬垂/垃圾指针）一律不可读，
// 与生产 game_memory_accessible 的 /proc/self/maps 判定语义一致。
bool game_memory_accessible(const void* address, size_t size, char permission) {
    (void)permission;
    const uintptr_t start = g_base;
    const uintptr_t addr = reinterpret_cast<uintptr_t>(address);
    return address != nullptr && size > 0 && addr >= start && size <= kFakeGameSize &&
           addr - start <= kFakeGameSize - size;
}

namespace {
int g_pass = 0;
int g_fail = 0;
int g_game_over_calls = 0;
int g_in_world = 1;
int g_story = 0;
int g_popup_pop_calls = 0;
int g_popup_push_calls = 0;
int g_main_menu_transition_calls = 0;
int g_game_state_set_calls = 0;
int32_t g_last_game_state = -1;
uint32_t* g_fake_popup_count = nullptr;
uintptr_t* g_fake_popup_enter = nullptr;
uint8_t* g_fake_state_list = nullptr;

#define CHECK(cond) do { \
    if (cond) ++g_pass; \
    else { ++g_fail; std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } \
} while (0)

int game_over() { ++g_game_over_calls; return 0; }

int g_enter_match = 0;
int g_prepare_mm_ok = 1;
void dummy_set_state(int32_t state) {
    ++g_game_state_set_calls;
    g_last_game_state = state;
}
int dummy_popup_info(int32_t action, int32_t state_id) {
    if (action == 3) {
        ++g_popup_pop_calls;
        return 0;
    }
    if (action == 1) {
        ++g_popup_push_calls;
        if (g_fake_popup_count != nullptr) *g_fake_popup_count = 1;
        if (g_fake_popup_enter != nullptr && g_fake_state_list != nullptr && state_id >= 0) {
            *g_fake_popup_enter = *reinterpret_cast<uintptr_t*>(
                g_fake_state_list + state_id * POPUP_ENTRY_SIZE + POPUP_ENTRY_ENTER);
        }
    }
    return 0;
}
QolResult run_transition_inline(qol::contracts::QolRuntimeTransitionBodyFn body, void* ctx,
                                char* out, uint32_t capacity, uint32_t* out_size) {
    ++g_main_menu_transition_calls;
    if (body == nullptr || out == nullptr || out_size == nullptr) {
        return qol::contracts::error(kQolErrInvalidArgument, "bad transition", 14);
    }
    body(ctx, out, capacity, out_size);
    return qol::contracts::ok();
}
int32_t dispatch_prepare_save() { return 1; }
int32_t dispatch_is_enter(uintptr_t enter) { return enter == 0x1234 && g_enter_match ? 1 : 0; }
int32_t dispatch_prepare_mm() { return g_prepare_mm_ok; }
int32_t dispatch_state_id() { return 7; }
}

bool game_in_world() { return g_in_world != 0; }

namespace qol::game {
bool story_active() { return g_story != 0; }
SaveWriteResult save_game() { return {}; }
std::string story_json() { return "{}"; }
}

std::string op_err(const char* msg) { return std::string("{\"ok\":false,\"error\":\"") + msg + "\"}"; }
std::string op_ok() { return "{\"ok\":true}"; }
std::string json_escape(const char* text) { return text == nullptr ? "" : text; }

uintptr_t popup_enter_alias_resolve(uintptr_t) { return kPopupEnterAliasNone; }

namespace qol::contracts {
const QolRuntimeHooksApi& qol_runtime_hooks() {
    static const QolRuntimeHooksApi kHooks = [] {
        QolRuntimeHooksApi hooks{};
        hooks.abi_version = {1, 0};
        hooks.transition_run_inline = &run_transition_inline;
        return hooks;
    }();
    return kHooks;
}
}  // namespace qol::contracts

void test_ui_write_frame_steps(uint32_t* stack_count, uintptr_t* top_enter,
                               uint8_t* state_list) {
    using namespace qol::contracts;
    using namespace qol::game;
    g_fake_popup_count = stack_count;
    g_fake_popup_enter = top_enter;
    g_fake_state_list = state_list;
    fn_ui_set_popup_process_info = &dummy_popup_info;
    fn_gamestate_set_state = &dummy_set_state;
    g_popup_pop_calls = 0;
    g_popup_push_calls = 0;
    g_main_menu_transition_calls = 0;
    g_game_state_set_calls = 0;

    char out[256]{};
    uint32_t out_size = 0;
    *stack_count = 1;
    *top_enter = g_base + F_PANEL_CHARACTER_INFO_ENTER;
    QolResult result = ui_state_write_step(100, 0, kQolUiWritePanelOpen, "inventory", out,
                                           sizeof(out), &out_size);
    CHECK(result.code == kQolErrNotReady && out_size == 0);
    CHECK(g_popup_pop_calls == 1 && g_popup_push_calls == 0);

    result = ui_state_write_step(101, 0, kQolUiWritePanelClose, "", out, sizeof(out), &out_size);
    CHECK(result.code == kQolErrNotReady && g_popup_pop_calls == 1);

    *stack_count = 0;
    result = ui_state_write_step(100, 1, kQolUiWritePanelOpen, "inventory", out, sizeof(out),
                                 &out_size);
    CHECK(succeeded(result) && std::string(out, out_size) == "{\"ok\":true}");
    CHECK(g_popup_pop_calls == 1 && g_popup_push_calls == 1 && *stack_count == 1);

    result = ui_state_write_step(101, 1, kQolUiWritePanelClose, "", out, sizeof(out), &out_size);
    CHECK(succeeded(result) && g_popup_pop_calls == 2);

    // A pending panel switch pops once, defers through 59 further attempts, then keeps the old error.
    *stack_count = 1;
    *top_enter = g_base + F_PANEL_INVENTORY_ENTER;
    const int pop_calls_before_timeout = g_popup_pop_calls;
    result = ui_state_write_step(200, 0, kQolUiWritePanelOpen, "skills", out, sizeof(out),
                                 &out_size);
    CHECK(result.code == kQolErrNotReady && g_popup_pop_calls == pop_calls_before_timeout + 1);
    for (uint32_t attempt = 1; attempt < 59; ++attempt) {
        result = ui_state_write_step(200, attempt, kQolUiWritePanelOpen, "skills", out,
                                     sizeof(out), &out_size);
        CHECK(result.code == kQolErrNotReady);
    }
    result = ui_state_write_step(200, 59, kQolUiWritePanelOpen, "skills", out, sizeof(out),
                                 &out_size);
    CHECK(result.code == kQolErrTimeout &&
          std::string(out, out_size) ==
              "{\"ok\":false,\"error\":\"panel close pending\"}");
    CHECK(g_popup_pop_calls == pop_calls_before_timeout + 1);

    // The timeout releases the operation; a later owner command can complete.
    *stack_count = 0;
    result = ui_state_write_step(201, 0, kQolUiWritePanelOpen, "skills", out, sizeof(out),
                                 &out_size);
    CHECK(succeeded(result) && g_popup_push_calls == 2);

    // Main-menu transition waits for the stack to clear, then invokes transition exactly once.
    *stack_count = 1;
    *top_enter = g_base + F_PANEL_INVENTORY_ENTER;
    result = ui_state_write_step(300, 0, kQolUiWriteMainMenu, "", out, sizeof(out), &out_size);
    CHECK(result.code == kQolErrNotReady && g_popup_pop_calls == pop_calls_before_timeout + 2);
    *stack_count = 0;
    result = ui_state_write_step(300, 1, kQolUiWriteMainMenu, "", out, sizeof(out), &out_size);
    CHECK(succeeded(result) && g_main_menu_transition_calls == 1);
    CHECK(g_game_state_set_calls == 1 && g_last_game_state == 4);
    *stack_count = 0;
    CHECK(ui_main_menu(false).str() ==
          "{\"ok\":false,\"error\":\"panel close pending\"}");
    CHECK(g_main_menu_transition_calls == 1 && g_game_state_set_calls == 1);

    *stack_count = 1;
    *top_enter = g_base + F_PANEL_INVENTORY_ENTER;
    result = ui_state_write_step(400, 0, kQolUiWriteMainMenu, "", out, sizeof(out), &out_size);
    CHECK(result.code == kQolErrNotReady);
    CHECK(succeeded(ui_state_write_cancel_pending(400)));
    result = ui_state_write_step(401, 0, kQolUiWritePanelClose, "", out, sizeof(out), &out_size);
    CHECK(succeeded(result));

    // Legacy synchronous callers do one frame's work and retain the pending error body.
    const int pop_calls_before_legacy = g_popup_pop_calls;
    CHECK(ui_panel_open("skills").str() ==
          "{\"ok\":false,\"error\":\"panel close pending\"}");
    CHECK(g_popup_pop_calls == pop_calls_before_legacy + 1);
}

char* fake_name_bad(void*) {
    return reinterpret_cast<char*>(static_cast<uintptr_t>(0x3100000021));
}
char* fake_name_good(void*) { return reinterpret_cast<char*>(g_base + 0x3f1200); }

// NPC 对话数据（UICHOICE / NPCTASKLIST）在对话结束后属于悬垂数据：面板打开期间不得进入
// npc 分支，各文本指针解引用前必须校验可读（真机 c18 v603 的背包崩溃回归）。
void test_dialog_content_pointer_guards(uint8_t* popup_on, uint32_t* stack_count,
                                        uintptr_t* top_enter) {
    using namespace qol::game;
    *popup_on = 0;  // 关掉 UIPopupMsg，走 world/NPC 分支

    auto* slot_data = reinterpret_cast<uint8_t*>(g_base + 0x3f0000);
    char* text_aaa = reinterpret_cast<char*>(g_base + 0x3f1000);
    char* text_bbb = reinterpret_cast<char*>(g_base + 0x3f1100);
    char* text_speaker = reinterpret_cast<char*>(g_base + 0x3f1200);
    std::strcpy(text_aaa, "AAA");
    std::strcpy(text_bbb, "BBB");
    std::strcpy(text_speaker, "S2");
    char* const dangling = reinterpret_cast<char*>(static_cast<uintptr_t>(0x3100000021));

    *reinterpret_cast<void**>(g_base + G_NPCTASKLIST_PDATA_VMA) = slot_data;
    *reinterpret_cast<uint8_t*>(g_base + G_NPCTASKLIST_COUNT_VMA) = 2;
    *reinterpret_cast<uint8_t*>(g_base + G_UICHOICE_COUNT_VMA) = 0;
    *reinterpret_cast<char**>(slot_data + 0 * 16 + 8) = text_aaa;
    *reinterpret_cast<char**>(slot_data + 1 * 16 + 8) = dangling;
    *reinterpret_cast<char**>(g_base + G_NPCTASKLIST_DESCTEXT_VMA) = dangling;
    *reinterpret_cast<void**>(g_base + G_PLAYER_NEAR_NPC_VMA) = nullptr;

    // 1) 背包面板打开：残留的 NPC 计数不得再走 npc 分支（本次真机崩溃场景）。
    *stack_count = 1;
    *top_enter = g_base + F_PANEL_INVENTORY_ENTER;
    const std::string panel = dialog_content_json();
    CHECK(panel.rfind("{\"type\":\"inventory\"", 0) == 0);
    CHECK(panel.find("\"type\":\"npc\"") == std::string::npos);

    // 2) 栈空数据态：仍输出 npc，但悬垂槽文本与 desc 被过滤，不崩溃。
    *stack_count = 0;
    const std::string residual = dialog_content_json();
    CHECK(residual.rfind("{\"type\":\"npc\"", 0) == 0);
    CHECK(residual.find("\"displayed\":false") != std::string::npos);
    CHECK(residual.find("\"label\":\"AAA\"") != std::string::npos);
    CHECK(residual.find("\"id\":\"1\",\"label\":\"\"") != std::string::npos);
    CHECK(residual.find("\"text\":") == std::string::npos);
    CHECK(residual.find("\"id\":\"close\"") != std::string::npos);

    // 3) NPC 对话面板：正常输出且 displayed=true。
    *stack_count = 1;
    *top_enter = g_base + F_PANEL_NPC_ENTER;
    const std::string npc = dialog_content_json();
    CHECK(npc.find("\"displayed\":true") != std::string::npos);
    CHECK(npc.find("\"label\":\"AAA\"") != std::string::npos);

    // 4) 槽数组本身不可读：整段按无选项处理，只保留 close。
    *reinterpret_cast<void**>(g_base + G_NPCTASKLIST_PDATA_VMA) = dangling;
    const std::string no_slots = dialog_content_json();
    CHECK(no_slots.find("\"label\":\"AAA\"") == std::string::npos);
    CHECK(no_slots.find("\"id\":\"close\"") != std::string::npos);
    *reinterpret_cast<void**>(g_base + G_NPCTASKLIST_PDATA_VMA) = slot_data;

    // 5) UICHOICE 选项文本同规则逐项校验。
    *reinterpret_cast<uint8_t*>(g_base + G_UICHOICE_COUNT_VMA) = 2;
    auto** choice_texts = reinterpret_cast<char**>(g_base + G_UICHOICE_ITEMTEXT_VMA);
    choice_texts[0] = dangling;
    choice_texts[1] = text_bbb;
    *top_enter = g_base + F_PANEL_CHOICE_ENTER;
    const std::string choice = dialog_content_json();
    CHECK(choice.find("\"id\":\"0\",\"label\":\"\"") != std::string::npos);
    CHECK(choice.find("\"id\":\"1\",\"label\":\"BBB\"") != std::string::npos);
    *reinterpret_cast<uint8_t*>(g_base + G_UICHOICE_COUNT_VMA) = 0;

    // 6) speaker 指针不可读时不输出 speaker，可读时正常输出。
    *reinterpret_cast<void**>(g_base + G_PLAYER_NEAR_NPC_VMA) = text_bbb;
    *top_enter = g_base + F_PANEL_NPC_ENTER;
    fn_get_name = &fake_name_bad;
    CHECK(dialog_content_json().find("\"speaker\":") == std::string::npos);
    fn_get_name = &fake_name_good;
    CHECK(dialog_content_json().find("\"speaker\":\"S2\"") != std::string::npos);
    fn_get_name = nullptr;
    *reinterpret_cast<void**>(g_base + G_PLAYER_NEAR_NPC_VMA) = nullptr;

    // 7) desc 可读时输出 text。
    *reinterpret_cast<char**>(g_base + G_NPCTASKLIST_DESCTEXT_VMA) = text_bbb;
    CHECK(dialog_content_json().find("\"text\":\"BBB\"") != std::string::npos);
    *reinterpret_cast<char**>(g_base + G_NPCTASKLIST_DESCTEXT_VMA) = nullptr;

    *stack_count = 1;
    *top_enter = g_base + F_PANEL_INVENTORY_ENTER;
    *popup_on = 1;
}

int main() {
    using namespace qol::game;
    auto* base = static_cast<uint8_t*>(mmap(nullptr, kFakeGameSize, PROT_READ | PROT_WRITE | PROT_EXEC,
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

    // ITM-13 批 2：生命周期参与者迁移后的 ui_state 三态（enter 识别 + main-menu 错误体）。
    const qol::contracts::QolLifecycleDispatch dispatch{
        &dispatch_prepare_save, &dispatch_is_enter, &dispatch_prepare_mm, &dispatch_state_id};
    CHECK(lifecycle_participant_bind_host(&dispatch));
    g_enter_match = 1;
    entry.enter = 0x1234;
    stack.count = 1;
    CHECK(popup_top_vma() == F_PANEL_INVENTORY_ENTER);
    g_enter_match = 0;
    CHECK(popup_top_vma() == 0);
    // main-menu 参与者失败：完整外层 JSON 逐字锁定，且切换未执行。
    stack.count = 0;
    g_prepare_mm_ok = 0;
    fn_gamestate_set_state = &dummy_set_state;
    fn_ui_set_popup_process_info = &dummy_popup_info;
    CHECK(ui_main_menu(true).str() ==
          "{\"ok\":false,\"error\":\"extension bag cleanup failed; main menu transition blocked\"}");
    g_prepare_mm_ok = 1;

    uint8_t state_list[27 * POPUP_ENTRY_SIZE]{};
    const uintptr_t inventory_enter = g_base + F_PANEL_INVENTORY_ENTER;
    const uintptr_t skills_enter = g_base + F_PANEL_SKILLS_ENTER;
    std::memcpy(state_list + 7 * POPUP_ENTRY_SIZE + POPUP_ENTRY_ENTER, &inventory_enter,
                sizeof(inventory_enter));
    std::memcpy(state_list + 6 * POPUP_ENTRY_SIZE + POPUP_ENTRY_ENTER, &skills_enter,
                sizeof(skills_enter));
    *reinterpret_cast<uint8_t**>(g_base + G_POPUP_STATE_LIST_GOT_VMA) = state_list;
    test_ui_write_frame_steps(&stack.count, &entry.enter, state_list);
    test_dialog_content_pointer_guards(&popup_on, &stack.count, &entry.enter);

    munmap(base, kFakeGameSize);
    std::printf("ui_state_dialog: pass=%d fail=%d\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
