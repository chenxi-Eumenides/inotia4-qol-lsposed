// core.ui 面板栈 host 测试（D1-b2）。
//
// 覆盖 panels/stack：open 幂等、push 成功、push 失败「关栈顶再开」兜底、陈旧栈顶剔除、
// 栈深 4 上限、close（含其上层）、block_reason 栈顶优先；以及帧泵剔除自关陈旧栈顶（P2）、
// 延迟关闭不提前弹栈（P3）与停滞逃生路径（P3b）。
//
// fake provider 经 Host SDK panels.find 提供；不触碰游戏内存/真实 UI。
// 设置页分页规则随旧静态页删除；新数据驱动模型由 settings_panel_model_tests 覆盖。

#include <cstdint>
#include <cstdio>
#include <cstring>

#include "contracts/panels.h"
#include "contracts/host_sdk.h"
#include "contracts/ui.h"
#include "plugins/core.custom_ui/panels/host.h"
#include "plugins/core.custom_ui/panels/stack.h"

using qol::contracts::PanelItemKind;
using qol::contracts::PanelProviderSpec;
using qol::contracts::QolHostSdk;
using qol::features::core_ui::ui_panels_active;
using qol::features::core_ui::ui_panels_back;
using qol::features::core_ui::ui_panels_bind_sdk;
using qol::features::core_ui::ui_panels_block_reason;
using qol::features::core_ui::ui_panels_close;
using qol::features::core_ui::ui_panels_depth_for_test;
using qol::features::core_ui::ui_panels_deactivate_owner;
using qol::features::core_ui::ui_panels_frame_pump;
using qol::features::core_ui::ui_panels_open;
using qol::features::core_ui::ui_panels_request_back;
using qol::features::core_ui::ui_panels_reset_for_test;

static int g_checks = 0;
static int g_failures = 0;

#define CHECK(cond)                                                        \
    do {                                                                   \
        ++g_checks;                                                        \
        if (!(cond)) {                                                     \
            ++g_failures;                                                  \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);    \
        }                                                                  \
    } while (0)

// ---------------- 面板栈 ----------------

namespace {

struct Fake {
    bool active = false;
    int open_calls = 0;
    int close_calls = 0;
    int fail_open_times = 0;
    bool delay_close = false;
    const char* reason = nullptr;
};

Fake g_f[5];
Fake g_rebound;
bool g_use_rebound_provider = false;
QolHostSdk* g_test_sdk = nullptr;
uint64_t g_panel_snapshot_sequence = 0;
uint32_t g_panel_snapshot_count = 0;
bool g_ui_state_capability_available = true;
char g_panel_snapshot_ids[qol::contracts::kUiStateMaxCustomPanels]
                         [qol::contracts::kUiStateMaxPanelIdBytes]{};

QolResult publish_panel_snapshot(const qol::contracts::UiCustomPanelsSnapshot* snapshot) {
    if (snapshot == nullptr || snapshot->count > qol::contracts::kUiStateMaxCustomPanels ||
        (snapshot->count > 0 && snapshot->panel_ids == nullptr)) {
        return qol::contracts::error(kQolErrInvalidArgument, "bad snapshot", 12);
    }
    g_panel_snapshot_sequence = snapshot->sequence;
    g_panel_snapshot_count = snapshot->count;
    for (uint32_t i = 0; i < snapshot->count; ++i) {
        std::snprintf(g_panel_snapshot_ids[i], sizeof(g_panel_snapshot_ids[i]), "%s",
                      snapshot->panel_ids[i]);
    }
    return qol::contracts::ok();
}

QolResult publish_dialog_snapshot(const qol::contracts::UiCustomDialogSnapshot*) {
    return qol::contracts::ok();
}

QolResult clear_dialog_snapshot(uint64_t, uint64_t) { return qol::contracts::ok(); }

const qol::contracts::UiStateApi kUiStateApi{
    &publish_panel_snapshot, &publish_dialog_snapshot, &clear_dialog_snapshot};

QolResult fake_capability_require(const char* id, QolVersionRange,
                                  const void** out_iface) {
    if (!g_ui_state_capability_available || id == nullptr || out_iface == nullptr ||
        std::strcmp(id, qol::contracts::kUiStateCapabilityId) != 0) {
        return qol::contracts::error(kQolErrNotFound, "not found", 9);
    }
    *out_iface = &kUiStateApi;
    return qol::contracts::ok();
}

bool f_open(int i) {
    Fake& f = g_f[i];
    ++f.open_calls;
    if (f.fail_open_times > 0) {
        --f.fail_open_times;
        f.active = false;
        return false;
    }
    f.active = true;
    return true;
}

void f_close(int i) {
    ++g_f[i].close_calls;
    if (!g_f[i].delay_close) g_f[i].active = false;
}

bool f_active(int i) { return g_f[i].active; }
const char* f_reason(int i) { return g_f[i].reason; }

bool a_open() { return f_open(0); }
void a_close() { f_close(0); }
bool a_active() { return f_active(0); }
const char* a_reason() { return f_reason(0); }

bool rebound_open() {
    g_rebound.active = true;
    ++g_rebound.open_calls;
    return true;
}
void rebound_close() {
    g_rebound.active = false;
    ++g_rebound.close_calls;
}
bool rebound_active() { return g_rebound.active; }
const char* rebound_reason() { return g_rebound.reason; }

bool b_open() { return f_open(1); }
void b_close() { f_close(1); }
bool b_active() { return f_active(1); }
const char* b_reason() { return f_reason(1); }
bool c_open() { return f_open(2); }
void c_close() { f_close(2); }
bool c_active() { return f_active(2); }
const char* c_reason() { return f_reason(2); }
bool d_open() { return f_open(3); }
void d_close() { f_close(3); }
bool d_active() { return f_active(3); }
const char* d_reason() { return f_reason(3); }
bool e_open() { return f_open(4); }
void e_close() { f_close(4); }
bool e_active() { return f_active(4); }
const char* e_reason() { return f_reason(4); }

// ---------------- callbacks provider + PopupHost 接缝（P3 退出链 / P3b 逃生） ----------------

bool g_pf_active = false;
int g_pf_open_calls = 0;
int g_pf_close_calls = 0;
const qol::contracts::QolPopupStateHooks* g_pf_popup_hooks = nullptr;
int g_pf_acquires = 0;
int g_pf_restores = 0;
int g_pf_pushes = 0;
int g_pf_pops = 0;
uint32_t g_pf_game_count = 1;  // 打开前的游戏栈 baseline
uintptr_t g_pf_game_top = 0x1234;
bool g_pf_snapshot_ok = true;
char g_pf_last_log[192] = {};

void pf_enter() {}
void pf_process() {}
void pf_f3() {}
void pf_f4() {}
uint32_t pf_event(uint64_t, uint64_t, uint64_t) { return 7; }
const qol::contracts::PanelCallbacks kPfCallbacks{&pf_enter, &pf_process, &pf_f3, &pf_f4, &pf_event};
bool pf_open() { ++g_pf_open_calls; g_pf_active = true; return true; }
void pf_close() { ++g_pf_close_calls; g_pf_active = false; }
bool pf_active() { return g_pf_active; }
const char* pf_reason() { return nullptr; }

QolResult pf_resolve(const char* symbol, qol::contracts::QolPopupStateEntry* out) {
    if (symbol == nullptr || out == nullptr || std::strcmp(symbol, "F_PANEL_UNK1_ENTER") != 0) {
        return qol::contracts::error(kQolErrInvalidArgument, "bad symbol", 7);
    }
    static uint8_t slot[0x40]{};
    out->slot = slot;
    out->state_id = 21;
    return qol::contracts::ok();
}
QolResult pf_acquire(qol::contracts::QolPopupStateEntry*, const qol::contracts::QolPopupStateHooks* hooks) {
    ++g_pf_acquires;
    g_pf_popup_hooks = hooks;
    return qol::contracts::ok();
}
QolResult pf_restore(qol::contracts::QolPopupStateEntry* entry) {
    ++g_pf_restores;
    entry->slot = nullptr;
    return qol::contracts::ok();
}
QolResult pf_set_process_info(int32_t process, int32_t state_id) {
    if (process == 1) {
        ++g_pf_pushes;
        if (state_id != 21) return qol::contracts::error(kQolErrInvalidArgument, "state", 7);
        ++g_pf_game_count;
        g_pf_game_top = 0x9999;
        if (g_pf_popup_hooks != nullptr) g_pf_popup_hooks->enter();
        return qol::contracts::ok();
    }
    ++g_pf_pops;
    if (process != 3 || state_id != 0) {
        return qol::contracts::error(kQolErrInvalidArgument, "pop args", 7);
    }
    if (g_pf_game_count > 0) --g_pf_game_count;
    if (g_pf_game_count == 1) g_pf_game_top = 0x1234;
    return qol::contracts::ok();
}
QolResult pf_snapshot(uint32_t* count, uintptr_t* top_enter_vma) {
    if (count == nullptr || top_enter_vma == nullptr || !g_pf_snapshot_ok) {
        return qol::contracts::error(kQolErrNotReady, "snapshot", 16);
    }
    *count = g_pf_game_count;
    *top_enter_vma = g_pf_game_top;
    return qol::contracts::ok();
}
QolResult pf_log(int32_t, int32_t, const char*, const char* message, uint32_t) {
    if (message != nullptr) std::snprintf(g_pf_last_log, sizeof(g_pf_last_log), "%s", message);
    return qol::contracts::ok();
}

const PanelProviderSpec kProviders[] = {
    {"pa", "core.settings", &a_open, &a_close, &a_active, &a_reason},
    {"pb", "save_backup", &b_open, &b_close, &b_active, &b_reason},
    {"pc", "core.ui", &c_open, &c_close, &c_active, &c_reason},
    {"pd", "core.ui", &d_open, &d_close, &d_active, &d_reason},
    {"pe", "core.ui", &e_open, &e_close, &e_active, &e_reason},
    {"pf", "core.ui", &pf_open, &pf_close, &pf_active, &pf_reason, &kPfCallbacks},
};

QolResult fake_panels_find(const char* panel_id, const PanelProviderSpec** out_provider) {
    if (panel_id == nullptr || out_provider == nullptr) {
        return qol::contracts::error(kQolErrInvalidArgument, "invalid", 7);
    }
    *out_provider = nullptr;
    static const PanelProviderSpec rebound_provider{
        "pa", "core.settings", &rebound_open, &rebound_close, &rebound_active, &rebound_reason};
    if (std::strcmp(panel_id, "pa") == 0 && g_use_rebound_provider) {
        *out_provider = &rebound_provider;
        return qol::contracts::ok();
    }
    for (const PanelProviderSpec& provider : kProviders) {
        if (std::strcmp(provider.panel_id, panel_id) == 0) {
            *out_provider = &provider;
            return qol::contracts::ok();
        }
    }
    return qol::contracts::error(kQolErrNotFound, "not found", 9);
}

void reset_fakes() {
    for (Fake& f : g_f) {
        f = Fake{};
    }
    g_rebound = Fake{};
    g_use_rebound_provider = false;
    ui_panels_reset_for_test();
}

void reset_popup_fakes() {
    g_pf_active = false;
    g_pf_open_calls = g_pf_close_calls = 0;
    g_pf_popup_hooks = nullptr;
    g_pf_acquires = g_pf_restores = g_pf_pushes = g_pf_pops = 0;
    g_pf_game_count = 1;
    g_pf_game_top = 0x1234;
    g_pf_snapshot_ok = true;
    g_pf_last_log[0] = '\0';
}

void test_stack_open_idempotent() {
    reset_fakes();
    CHECK(ui_panels_open("pa"));
    CHECK(g_f[0].open_calls == 1 && g_f[0].active);
    CHECK(ui_panels_depth_for_test() == 1);
    CHECK(g_panel_snapshot_count == 1 &&
          std::strcmp(g_panel_snapshot_ids[0], "pa") == 0);
    // 已在栈顶且 active → 幂等，不再调 provider.open。
    CHECK(ui_panels_open("pa"));
    CHECK(g_f[0].open_calls == 1);
    CHECK(ui_panels_active("pa"));
    CHECK(!ui_panels_active("pb"));
    CHECK(!ui_panels_active(nullptr));
    CHECK(!ui_panels_open("missing"));
}

void test_stack_fallback_close_top() {
    reset_fakes();
    CHECK(ui_panels_open("pa"));  // 设置页在栈
    g_f[1].fail_open_times = 1;   // 备份面板首推失败（模拟嵌套 push 未切换栈顶）
    CHECK(ui_panels_open("pb"));
    CHECK(g_f[0].close_calls == 1);   // 关掉了栈顶设置页
    CHECK(g_f[1].open_calls == 2);    // 关闭后重试成功
    CHECK(g_f[1].active);
    CHECK(ui_panels_depth_for_test() == 1);

    // 无面板 active 时 block_reason 为空。
    g_f[1].active = false;
    CHECK(ui_panels_block_reason() == nullptr);
}

void test_stack_block_reason_top_first() {
    reset_fakes();
    CHECK(ui_panels_open("pa"));
    g_f[0].reason = "ui occupied: settings panel";
    CHECK(ui_panels_open("pb"));
    g_f[1].reason = "ui occupied: backup panel";
    CHECK(ui_panels_depth_for_test() == 2);
    // 栈顶优先：嵌套时报告最上层。
    CHECK(std::strcmp(ui_panels_block_reason(), "ui occupied: backup panel") == 0);
    ui_panels_close("pb");
    CHECK(g_f[0].active);
    CHECK(std::strcmp(ui_panels_block_reason(), "ui occupied: settings panel") == 0);
}

void test_stack_nested_back_order() {
    reset_fakes();
    CHECK(ui_panels_open("pa"));
    CHECK(ui_panels_open("pb"));
    CHECK(ui_panels_open("pc"));
    CHECK(ui_panels_depth_for_test() == 3);
    CHECK(g_panel_snapshot_count == 3 &&
          std::strcmp(g_panel_snapshot_ids[0], "pa") == 0 &&
          std::strcmp(g_panel_snapshot_ids[1], "pb") == 0 &&
          std::strcmp(g_panel_snapshot_ids[2], "pc") == 0);
    CHECK(ui_panels_back());
    CHECK(ui_panels_depth_for_test() == 2 && !g_f[2].active && g_f[0].active && g_f[1].active);
    CHECK(g_panel_snapshot_count == 2 && std::strcmp(g_panel_snapshot_ids[1], "pb") == 0);
    CHECK(ui_panels_back());
    CHECK(ui_panels_depth_for_test() == 1 && !g_f[1].active && g_f[0].active);
    ui_panels_close("pa");
    CHECK(ui_panels_depth_for_test() == 0 && !g_f[0].active);
    CHECK(!ui_panels_back());
}

void test_stack_prune_stale_top() {
    reset_fakes();
    CHECK(ui_panels_open("pa"));
    g_f[0].active = false;  // provider 自行关闭（游戏内返回键），不经栈
    CHECK(ui_panels_open("pb"));
    CHECK(g_f[0].close_calls == 0);  // 陈旧栈顶只丢记录，不再调 close
    CHECK(g_f[1].active);
    CHECK(ui_panels_depth_for_test() == 1);
}

void test_stack_depth_limit_and_close() {
    reset_fakes();
    CHECK(ui_panels_open("pa"));
    CHECK(ui_panels_open("pb"));
    CHECK(ui_panels_open("pc"));
    CHECK(ui_panels_open("pd"));
    CHECK(ui_panels_depth_for_test() == 4);
    CHECK(ui_panels_open("pe"));  // 深度上限：关栈顶再开
    CHECK(ui_panels_depth_for_test() == 4);
    CHECK(g_f[3].close_calls == 1);  // pd 被关
    CHECK(g_f[4].active);
    CHECK(g_f[0].active && g_f[1].active && g_f[2].active);

    // close 顶层：只关自己。
    ui_panels_close("pe");
    CHECK(!g_f[4].active);
    CHECK(ui_panels_depth_for_test() == 3);

    // close 底层：连同其上的面板一并关闭。
    ui_panels_close("pa");
    CHECK(g_f[0].close_calls == 1 && g_f[1].close_calls == 1 && g_f[2].close_calls == 1);
    CHECK(ui_panels_depth_for_test() == 0);
    CHECK(!g_f[0].active && !g_f[1].active && !g_f[2].active);

    // 未在栈中但 active 的 provider：直接 close。
    g_f[4].active = true;
    ui_panels_close("pe");
    CHECK(g_f[4].close_calls == 2);
}

void test_provider_deactivate_removes_owned_panel() {
    reset_fakes();
    CHECK(ui_panels_open("pa"));
    CHECK(ui_panels_open("pb"));
    CHECK(ui_panels_deactivate_owner("core.settings"));
    CHECK(!g_f[0].active && g_f[1].active);
    CHECK(g_f[0].close_calls == 1 && g_f[1].close_calls == 0);
    CHECK(ui_panels_depth_for_test() == 1);
    CHECK(g_panel_snapshot_count == 1 && std::strcmp(g_panel_snapshot_ids[0], "pb") == 0);
    CHECK(ui_panels_deactivate_owner("core.settings"));
    CHECK(g_f[0].close_calls == 1);
    CHECK(!ui_panels_deactivate_owner(nullptr));
}

void test_failed_open_does_not_publish() {
    reset_fakes();
    const uint64_t empty_sequence = g_panel_snapshot_sequence;
    g_f[0].fail_open_times = 1;
    CHECK(!ui_panels_open("pa"));
    CHECK(ui_panels_depth_for_test() == 0 && g_panel_snapshot_count == 0);
    CHECK(g_panel_snapshot_sequence == empty_sequence);
}

void test_optional_state_registry_does_not_block_native_panel() {
    reset_fakes();
    const uint64_t sequence_before = g_panel_snapshot_sequence;
    g_ui_state_capability_available = false;
    CHECK(ui_panels_open("pa"));
    CHECK(g_f[0].active && ui_panels_depth_for_test() == 1);
    CHECK(g_panel_snapshot_sequence == sequence_before);
    CHECK(g_panel_snapshot_count == 0);  // missing optional owner receives no invented state
    g_ui_state_capability_available = true;
}

void test_pending_provider_close_keeps_published_panel() {
    reset_fakes();
    g_f[0].delay_close = true;
    CHECK(ui_panels_open("pa"));
    const uint64_t open_sequence = g_panel_snapshot_sequence;
    CHECK(!ui_panels_back());
    CHECK(ui_panels_depth_for_test() == 1 && g_panel_snapshot_count == 1);
    CHECK(std::strcmp(g_panel_snapshot_ids[0], "pa") == 0);
    CHECK(g_panel_snapshot_sequence == open_sequence);
    g_f[0].active = false;
    CHECK(ui_panels_frame_pump(1, nullptr) == 1);
    CHECK(ui_panels_depth_for_test() == 0 && g_panel_snapshot_count == 0);
}

void test_owner_unregistration_waits_for_close() {
    reset_fakes();
    g_f[0].delay_close = true;
    CHECK(ui_panels_open("pa"));
    const uint64_t open_sequence = g_panel_snapshot_sequence;
    CHECK(!ui_panels_deactivate_owner("core.settings"));
    CHECK(ui_panels_depth_for_test() == 1 && g_panel_snapshot_count == 1);
    CHECK(g_panel_snapshot_sequence == open_sequence);
    g_f[0].active = false;
    CHECK(ui_panels_deactivate_owner("core.settings"));
    CHECK(ui_panels_depth_for_test() == 0 && g_panel_snapshot_count == 0);
}

void test_unbind_pending_preserves_old_generation_and_rebinds_new_id() {
    reset_fakes();
    g_f[0].delay_close = true;
    CHECK(ui_panels_open("pa"));
    const uint64_t old_sequence = g_panel_snapshot_sequence;
    ui_panels_bind_sdk(nullptr);
    CHECK(ui_panels_depth_for_test() == 1 && g_panel_snapshot_count == 1);
    CHECK(g_panel_snapshot_sequence == old_sequence);

    g_use_rebound_provider = true;
    g_rebound.active = true;
    CHECK(!ui_panels_open("pa"));  // pending old close cannot be mistaken for the new provider.
    g_f[0].active = false;
    CHECK(ui_panels_frame_pump(1, nullptr) == 1);
    CHECK(ui_panels_depth_for_test() == 0 && g_panel_snapshot_count == 0);

    ui_panels_bind_sdk(g_test_sdk);
    CHECK(ui_panels_open("pa"));
    CHECK(g_rebound.active && ui_panels_depth_for_test() == 1);
    const uint64_t new_sequence = g_panel_snapshot_sequence;
    CHECK(new_sequence > old_sequence && std::strcmp(g_panel_snapshot_ids[0], "pa") == 0);
    CHECK(ui_panels_frame_pump(2, nullptr) == 1);
    CHECK(g_panel_snapshot_count == 1 && std::strcmp(g_panel_snapshot_ids[0], "pa") == 0);
}

void test_owner_unregister_popup_pending_and_same_id_reopen() {
    reset_fakes();
    reset_popup_fakes();
    CHECK(ui_panels_open("pf"));
    g_pf_game_count = 5;
    g_pf_game_top = 0x7777;
    CHECK(!ui_panels_deactivate_owner("core.ui"));
    const uint64_t pending_sequence = g_panel_snapshot_sequence;
    CHECK(ui_panels_depth_for_test() == 1 && g_panel_snapshot_count == 1);
    CHECK(std::strcmp(g_panel_snapshot_ids[0], "pf") == 0);
    CHECK(!ui_panels_open("pf"));

    g_pf_game_count = 2;
    g_pf_game_top = 0x9999;
    for (int frame = 1; frame <= 5 && ui_panels_depth_for_test() != 0; ++frame) {
        CHECK(ui_panels_frame_pump(frame, nullptr) == 1);
    }
    CHECK(g_pf_close_calls == 1 && ui_panels_depth_for_test() == 0);
    CHECK(g_panel_snapshot_count == 0 && g_panel_snapshot_sequence > pending_sequence);

    CHECK(ui_panels_open("pf"));  // Re-register/open same id after the old close completed.
    CHECK(ui_panels_depth_for_test() == 1 && g_panel_snapshot_count == 1);
    CHECK(std::strcmp(g_panel_snapshot_ids[0], "pf") == 0);
    CHECK(ui_panels_frame_pump(6, nullptr) == 1);
    CHECK(g_panel_snapshot_count == 1 && std::strcmp(g_panel_snapshot_ids[0], "pf") == 0);
    ui_panels_close("pf");
    CHECK(ui_panels_frame_pump(7, nullptr) == 1);
    CHECK(ui_panels_frame_pump(8, nullptr) == 1);
    CHECK(ui_panels_frame_pump(9, nullptr) == 1);
    CHECK(ui_panels_depth_for_test() == 0 && g_panel_snapshot_count == 0);
}

// P2 栈深语义：provider 自关（存档备份返回键自调 set_process_info(3,0)，不经本栈）后
// 静置，帧泵开头剔除陈旧栈顶 → depth 回落，auto 导航按钮恢复画 X
// （depth1→X / depth2→箭头的绘制语义由 nav_button_component_tests 覆盖）。
void test_frame_pump_prunes_stale_top() {
    reset_fakes();
    CHECK(ui_panels_open("pa"));
    CHECK(ui_panels_open("pb"));
    CHECK(ui_panels_depth_for_test() == 2);
    g_f[1].active = false;  // provider 自关，不经栈
    CHECK(ui_panels_frame_pump(1, nullptr) == 1);
    CHECK(ui_panels_depth_for_test() == 1);  // 帧泵剔除陈旧栈顶
    CHECK(g_f[1].close_calls == 0);          // 只丢记录，不重复 close
    CHECK(g_f[0].active);
    // 深度已回落：再开 pb 幂等语义正确（不把陈旧记录当栈顶）。
    g_f[1].fail_open_times = 0;
    CHECK(ui_panels_open("pb"));
    CHECK(ui_panels_depth_for_test() == 2);
}

// P3 退出链：快照失配触发延迟关闭时，栈记录不得被提前弹出（缺陷 α）；
// queued 期间重复点击不吞请求、不重复发 pop；快照恢复后帧泵驱动完成关闭，
// pop 恰一次，深度回落 0。
void test_deferred_close_keeps_depth_until_done() {
    reset_fakes();
    reset_popup_fakes();
    CHECK(ui_panels_open("pf"));
    CHECK(g_pf_active && ui_panels_depth_for_test() == 1);
    CHECK(g_pf_pushes == 1 && g_pf_acquires == 1 && g_pf_pops == 0);
    // owned 快照 = push 后的 {2,0x9999}；制造游戏栈失配（真机退出失效现场）。
    g_pf_game_count = 5;
    g_pf_game_top = 0x7777;

    // 点击 X：request_back → 帧泵 back → 宿主延迟排队（close 返回 true 但未真正关闭）。
    ui_panels_request_back();
    CHECK(ui_panels_frame_pump(1, nullptr) == 1);
    CHECK(ui_panels_depth_for_test() == 1);  // 栈记录不提前弹出（缺陷 α 修复点）
    CHECK(g_panel_snapshot_count == 1 && std::strcmp(g_panel_snapshot_ids[0], "pf") == 0);
    CHECK(g_pf_close_calls == 0);
    CHECK(g_pf_pops == 0);

    // queued 期间重复点击：请求不被吞掉（仍可排队/维持），也不重复发 pop。
    ui_panels_request_back();
    CHECK(ui_panels_frame_pump(2, nullptr) == 1);
    CHECK(ui_panels_depth_for_test() == 1 && g_pf_close_calls == 0 && g_pf_pops == 0);

    // 快照恢复（游戏栈回到 owned）：无需再次点击，帧泵重试完成 close + pop + restore。
    g_pf_game_count = 2;
    g_pf_game_top = 0x9999;
    CHECK(ui_panels_frame_pump(3, nullptr) == 1);
    CHECK(g_pf_close_calls == 1 && g_pf_pops == 1);
    CHECK(ui_panels_frame_pump(4, nullptr) == 1);
    CHECK(ui_panels_frame_pump(5, nullptr) == 1);
    CHECK(g_pf_restores == 1);
    // 关闭完成后 provider inactive → 帧泵剔除记录，深度归 0（游戏回 main_menu）。
    CHECK(ui_panels_depth_for_test() == 0);
    CHECK(g_panel_snapshot_count == 0);
    // 之后点击：栈空，干净 no-op（不崩溃、不再发 pop）。
    CHECK(!ui_panels_back());
    CHECK(g_pf_pops == 1);
}

// P3b 逃生路径：游戏栈与 owned 快照持续失配时守卫会无限重排队；停滞连续
// kCloseStallFrameLimit 帧后强制走完关闭（补 close + 补发一次 pop + 释放 popup entry +
// 清空状态机），日志记录超限原因与上限，且收尾后不再循环。
void test_close_stall_escape_forces_completion() {
    reset_fakes();
    reset_popup_fakes();
    CHECK(ui_panels_open("pf"));
    CHECK(ui_panels_depth_for_test() == 1);
    g_pf_game_count = 5;  // 持续失配
    g_pf_game_top = 0x7777;
    ui_panels_request_back();
    CHECK(ui_panels_frame_pump(1, nullptr) == 1);
    CHECK(ui_panels_depth_for_test() == 1 && g_pf_close_calls == 0);

    using qol::features::core_ui::PopupHost;
    int pumps = 1;
    while (g_pf_close_calls == 0 && pumps < PopupHost::kCloseStallFrameLimit * 2) {
        CHECK(ui_panels_frame_pump(++pumps, nullptr) == 1);
    }
    // 第 1 帧记录停滞基线，其后每帧 +1 → 强制发生在第 limit+1 帧（有界，不无限循环）。
    CHECK(g_pf_close_calls == 1);
    CHECK(pumps == PopupHost::kCloseStallFrameLimit + 1);
    CHECK(g_pf_pops == 1);      // pop 恰补发一次
    CHECK(g_pf_restores == 1);  // popup entry 尽力释放
    // 超限原因与 N 值写入日志（状态可见）。
    CHECK(std::strstr(g_pf_last_log, "close force-complete") != nullptr);
    CHECK(std::strstr(g_pf_last_log, "panel=pf") != nullptr);
    CHECK(std::strstr(g_pf_last_log, "limit=120") != nullptr);
    // 收尾后状态干净：继续泵帧剔除栈记录，无额外 close/pop，点击为干净 no-op。
    CHECK(ui_panels_frame_pump(++pumps, nullptr) == 1);
    CHECK(ui_panels_depth_for_test() == 0);
    CHECK(g_pf_close_calls == 1 && g_pf_pops == 1);
    CHECK(!ui_panels_back());
}

}  // namespace

int main() {
    QolHostSdk sdk{};
    sdk.panels.find = &fake_panels_find;
    qol::contracts::QolUiAdapter ui{};
    ui.set_popup_process_info = &pf_set_process_info;
    ui.popup_stack_snapshot = &pf_snapshot;
    sdk.ui = &ui;
    sdk.hooks.popup_resolve = &pf_resolve;
    sdk.hooks.popup_acquire = &pf_acquire;
    sdk.hooks.popup_restore = &pf_restore;
    sdk.log.write = &pf_log;
    sdk.capabilities.require = &fake_capability_require;
    g_test_sdk = &sdk;
    qol::features::core_ui::ui_panels_bind_sdk(&sdk);

    test_stack_open_idempotent();
    test_stack_fallback_close_top();
    test_stack_block_reason_top_first();
    test_stack_nested_back_order();
    test_stack_prune_stale_top();
    test_stack_depth_limit_and_close();
    test_provider_deactivate_removes_owned_panel();
    test_failed_open_does_not_publish();
    test_optional_state_registry_does_not_block_native_panel();
    test_pending_provider_close_keeps_published_panel();
    test_owner_unregistration_waits_for_close();
    test_unbind_pending_preserves_old_generation_and_rebinds_new_id();
    test_owner_unregister_popup_pending_and_same_id_reopen();
    test_frame_pump_prunes_stale_top();
    test_deferred_close_keeps_depth_until_done();
    test_close_stall_escape_forces_completion();

    std::printf("core_ui_panels_tests: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
