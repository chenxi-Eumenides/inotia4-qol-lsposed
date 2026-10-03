#include "plugins/core.custom_ui/panels/host.h"

#include <cassert>
#include <cstdint>
#include <cstring>

using qol::contracts::PanelCallbacks;
using qol::contracts::PanelProviderSpec;
using qol::contracts::QolPopupStateEntry;
using qol::contracts::QolPopupStateHooks;
using qol::features::core_ui::PopupHost;

namespace {

int g_resolves = 0;
int g_acquires = 0;
int g_restores = 0;
int g_process_calls = 0;
int g_last_process = -1;
int g_last_state_id = -1;
bool g_acquire_ok = true;
bool g_resolve_ok = true;
bool g_snapshot_ok = true;
int g_snapshot_reads = 0;
bool g_a_active = false;
bool g_b_active = false;
int g_open_a = 0;
int g_open_b = 0;
int g_close_a = 0;
int g_close_b = 0;
bool g_c_active = false;
int g_open_c = 0;
int g_close_c = 0;
int g_slots[5] = {};
uint32_t g_popup_count = 1;
uintptr_t g_popup_top = 0x1234;
uint32_t g_pending_count = 0;
uintptr_t g_pending_top = 0;
int g_logs = 0;
bool g_delay_pop = false;
uint32_t g_event_result = 0;
const QolPopupStateHooks* g_acquired_hooks = nullptr;
bool g_sync_enter_on_push = false;
PopupHost* g_deactivate_during_event = nullptr;

bool resolve_popup(void*, const char*, QolPopupStateEntry* entry) {
    ++g_resolves;
    if (!g_resolve_ok) return false;
    static uint8_t slot[0x40]{};
    entry->slot = slot;
    entry->state_id = 9;
    return true;
}

bool acquire_popup(void*, QolPopupStateEntry*, const QolPopupStateHooks* hooks) {
    ++g_acquires;
    if (!g_acquire_ok) return false;
    assert(hooks != nullptr);
    g_acquired_hooks = hooks;
    return true;
}

bool restore_popup(void*, QolPopupStateEntry* entry) {
    ++g_restores;
    entry->slot = nullptr;
    entry->state_id = -1;
    return true;
}

bool popup_stack_snapshot(void*, uint32_t* count, uintptr_t* top_enter_vma) {
    ++g_snapshot_reads;
    if (!g_snapshot_ok || count == nullptr || top_enter_vma == nullptr) return false;
    *count = g_popup_count;
    *top_enter_vma = g_popup_top;
    return true;
}

void host_log(void*, int32_t, const char*) { ++g_logs; }

bool set_process_info(void*, int32_t process, int32_t state_id) {
    ++g_process_calls;
    g_last_process = process;
    g_last_state_id = state_id;
    if (process == 1) {
        ++g_popup_count;
        g_popup_top = 0x9999;
    } else if (process == 3) {
        if (g_delay_pop) {
            g_delay_pop = false;
        } else if (g_pending_count != 0) {
            g_popup_count = g_pending_count;
            g_popup_top = g_pending_top;
            g_pending_count = 0;
        } else if (g_popup_count > 0) {
            --g_popup_count;
            if (g_popup_count == 1) g_popup_top = 0x1234;
        }
    }
    if (process == 1 && g_sync_enter_on_push && g_acquired_hooks != nullptr) {
        g_acquired_hooks->enter();
    }
    return true;
}

void a_enter() { ++g_slots[0]; }
void a_process() { ++g_slots[1]; }
void a_f3() { ++g_slots[2]; }
void a_f4() { ++g_slots[3]; }
uint32_t a_event(uint64_t, uint64_t, uint64_t) {
    ++g_slots[4];
    if (g_deactivate_during_event != nullptr) {
        PopupHost* host = g_deactivate_during_event;
        g_deactivate_during_event = nullptr;
        assert(host->deactivate_provider("owner.a", true));
        assert(host->close_pending_for_test());
    }
    return 41;
}
void b_enter() { g_slots[0] += 10; }
void b_process() { g_slots[1] += 10; }
void b_f3() { g_slots[2] += 10; }
void b_f4() { g_slots[3] += 10; }
uint32_t b_event(uint64_t, uint64_t, uint64_t) { g_slots[4] += 10; return 42; }

bool open_a() { ++g_open_a; g_a_active = true; return true; }
bool fail_open() { ++g_open_a; return false; }
bool open_b() { ++g_open_b; g_b_active = true; return true; }
bool open_c() { ++g_open_c; g_c_active = true; return true; }
void close_a() { ++g_close_a; g_a_active = false; }
void close_b() { ++g_close_b; g_b_active = false; }
void close_c() { ++g_close_c; g_c_active = false; }
bool active_a() { return g_a_active; }
bool active_b() { return g_b_active; }
bool active_c() { return g_c_active; }
const char* reason() { return nullptr; }

const PanelCallbacks kCallbacksA{a_enter, a_process, a_f3, a_f4, a_event};
const PanelCallbacks kCallbacksB{b_enter, b_process, b_f3, b_f4, b_event};
const PanelProviderSpec kPanelA{"panel.a", "owner.a", open_a, close_a, active_a, reason, &kCallbacksA};
const PanelProviderSpec kPanelB{"panel.b", "owner.b", open_b, close_b, active_b, reason, &kCallbacksB};
const PanelProviderSpec kPanelC{"panel.c", "owner.c", open_c, close_c, active_c, reason, &kCallbacksB};

void reset_fakes() {
    g_resolves = g_acquires = g_restores = 0;
    g_process_calls = 0;
    g_last_process = g_last_state_id = -1;
    g_acquired_hooks = nullptr;
    g_sync_enter_on_push = false;
    g_deactivate_during_event = nullptr;
    g_acquire_ok = g_resolve_ok = true;
    g_snapshot_ok = true;
    g_snapshot_reads = 0;
    g_a_active = g_b_active = g_c_active = false;
    g_popup_count = 1;
    g_popup_top = 0x1234;
    g_pending_count = 0;
    g_pending_top = 0;
    g_logs = 0;
    g_delay_pop = false;
    g_open_a = g_open_b = g_open_c = g_close_a = g_close_b = g_close_c = 0;
    std::memset(g_slots, 0, sizeof(g_slots));
}

PopupHost make_host() {
    return PopupHost("TEST_POPUP", {nullptr, resolve_popup, acquire_popup, restore_popup,
                                     set_process_info, popup_stack_snapshot, host_log});
}

}  // namespace

int main() {
    reset_fakes();
    auto host = make_host();
    assert(host.open(kPanelA, true));
    assert(host.open(kPanelA, true));
    assert(g_open_a == 1 && g_resolves == 1 && g_acquires == 1);
    assert(g_process_calls == 1 && g_last_process == 1 && g_last_state_id == 9);
    const auto* dispatch = host.dispatchers_for_test();
    dispatch->enter(); dispatch->process(); dispatch->f3(); dispatch->f4();
    assert(dispatch->event(1, 2, 3) == 41);
    assert(g_slots[0] == 1 && g_slots[1] == 1 && g_slots[2] == 1 && g_slots[3] == 1 && g_slots[4] == 1);

    assert(host.open(kPanelB, true));
    assert(g_close_a == 0 && g_open_b == 1 && g_acquires == 1);
    assert(g_popup_count == 2 && g_process_calls == 1);
    assert(g_a_active && g_b_active);
    assert(host.open(kPanelC, true));
    assert(g_close_b == 0 && g_open_c == 1 && host.current_panel_for_test() == "panel.c");
    dispatch->enter();
    assert(g_slots[0] == 31);
    assert(host.close("panel.c", true));
    assert(g_close_c == 1 && g_open_b == 2 && host.current_panel_for_test() == "panel.b");
    assert(host.close("panel.b", true));
    assert(g_close_b == 1 && g_open_a == 2 && host.current_panel_for_test() == "panel.a");
    assert(g_close_a == 0 && g_a_active && g_popup_count == 2 && g_process_calls == 1);

    assert(host.close("panel.a", true));
    assert(!host.close("panel.a", true));
    assert(g_restores == 0 && host.close_pending_for_test());
    assert(host.pump(true));
    assert(g_restores == 0 && g_last_process == 3);
    assert(host.pump(true));
    assert(host.pump(true));
    assert(g_restores == 1 && !host.close_pending_for_test());
    assert(host.open(kPanelA, true) && g_acquires == 2);
    assert(host.close("panel.a", true) && g_restores == 1);
    assert(host.pump(true)); assert(host.pump(true)); assert(host.pump(true));
    assert(host.pump(true));
    assert(g_restores == 2 && !host.close_pending_for_test());

    reset_fakes();
    auto unexpected_stack = make_host();
    assert(unexpected_stack.open(kPanelA, true));
    g_popup_count = 3;
    g_popup_top = 0x7777;
    assert(unexpected_stack.close("panel.a", true));
    assert(unexpected_stack.active_for_test() && g_close_a == 0);
    const int unexpected_process_calls = g_process_calls;
    assert(unexpected_stack.pump(true));
    assert(unexpected_stack.active_for_test() && g_restores == 0);
    assert(g_process_calls == unexpected_process_calls);

    reset_fakes();
    auto baseline_snapshot_failed = make_host();
    g_snapshot_ok = false;
    assert(!baseline_snapshot_failed.open(kPanelA, true));
    assert(!baseline_snapshot_failed.active_for_test());
    assert(g_close_a == 0 && g_restores == 0 && g_process_calls == 0);

    reset_fakes();
    // 非主线程 open 必须失败，且不得读取游戏 popup 栈快照。
    auto off_main = make_host();
    assert(!off_main.open(kPanelA, false));
    assert(!off_main.active_for_test());
    assert(g_snapshot_reads == 0 && g_process_calls == 0 && g_resolves == 0);
    assert(!off_main.close("panel.a", false));
    assert(!off_main.deactivate_provider("owner.a", false));
    assert(g_snapshot_reads == 0);

    reset_fakes();
    auto deactivate_top = make_host();
    assert(deactivate_top.open(kPanelA, true));
    assert(deactivate_top.open(kPanelB, true));
    assert(deactivate_top.deactivate_provider("owner.b", true));
    assert(std::strcmp(deactivate_top.current_panel_for_test(), "panel.a") == 0);
    assert(g_close_b == 1 && g_open_a == 2 && g_restores == 0);

    reset_fakes();
    auto deactivate_non_top = make_host();
    assert(deactivate_non_top.open(kPanelA, true));
    assert(deactivate_non_top.open(kPanelB, true));
    assert(deactivate_non_top.deactivate_provider("owner.a", true));
    assert(std::strcmp(deactivate_non_top.current_panel_for_test(), "panel.b") == 0);
    assert(g_close_b == 0 && g_restores == 0);
    assert(deactivate_non_top.deactivate_provider("owner.a", true));
    assert(deactivate_non_top.current_panel_for_test() != nullptr);

    reset_fakes();
    auto deactivate_last = make_host();
    assert(deactivate_last.open(kPanelA, true));
    assert(deactivate_last.deactivate_provider("owner.a", true));
    assert(!deactivate_last.active_for_test() && deactivate_last.close_pending_for_test());
    assert(deactivate_last.pump(true)); assert(deactivate_last.pump(true));
    assert(deactivate_last.pump(true));
    assert(g_close_a == 1 && g_restores == 1);
    assert(g_popup_count == 1 && g_popup_top == 0x1234);

    reset_fakes();
    auto retry_snapshot_deactivate = make_host();
    assert(retry_snapshot_deactivate.open(kPanelA, true));
    g_snapshot_ok = false;
    assert(retry_snapshot_deactivate.deactivate_provider("owner.a", true));
    assert(retry_snapshot_deactivate.pending_deactivate_for_owner("owner.a"));
    assert(retry_snapshot_deactivate.active_for_test());
    assert(g_close_a == 0 && g_restores == 0);
    g_snapshot_ok = true;
    assert(retry_snapshot_deactivate.pump(true));
    assert(retry_snapshot_deactivate.pump(true));
    assert(retry_snapshot_deactivate.pump(true));
    assert(!retry_snapshot_deactivate.active_for_test());
    assert(!retry_snapshot_deactivate.close_pending_for_test());
    assert(g_close_a == 1 && g_restores == 1);

    reset_fakes();
    auto retry_snapshot_close = make_host();
    assert(retry_snapshot_close.open(kPanelA, true));
    g_snapshot_ok = false;
    assert(retry_snapshot_close.close("panel.a", true));
    assert(retry_snapshot_close.active_for_test());
    assert(g_close_a == 0 && g_restores == 0);
    g_snapshot_ok = true;
    assert(retry_snapshot_close.pump(true));
    assert(retry_snapshot_close.pump(true));
    assert(retry_snapshot_close.pump(true));
    assert(!retry_snapshot_close.active_for_test());
    assert(g_close_a == 1 && g_restores == 1);

    reset_fakes();
    // 原生面板先占一层，模块设置弹层再压在其上；deactivate 只 pop 模块 entry。
    g_popup_count = 2;
    g_popup_top = 0x5678;
    auto native_under_module = make_host();
    assert(native_under_module.open(kPanelA, true));
    assert(g_popup_count == 3 && g_popup_top == 0x9999);
    g_delay_pop = true;
    assert(native_under_module.deactivate_provider("owner.a", true));
    assert(native_under_module.close_pending_for_test() && g_restores == 0);
    assert(g_popup_count == 3 && g_popup_top == 0x9999);
    const int one_pop_only = g_process_calls;
    assert(!native_under_module.deactivate_provider("owner.a", true));
    assert(g_process_calls == one_pop_only && g_restores == 0);
    // 游戏帧提交一次 pop 后，原生面板仍在栈顶，才能还原 host hook。
    g_popup_count = 2;
    g_popup_top = 0x5678;
    assert(native_under_module.pump(true));
    assert(g_restores == 0 && native_under_module.close_pending_for_test());
    assert(native_under_module.pump(true));
    assert(g_restores == 1 && !native_under_module.close_pending_for_test());
    assert(g_popup_count == 2 && g_popup_top == 0x5678);

    reset_fakes();
    auto delayed_pop = make_host();
    assert(delayed_pop.open(kPanelA, true));
    // 模拟一次 pop 已发起但游戏帧尚未提交：旧模块 entry 仍在栈顶。
    g_delay_pop = true;
    assert(delayed_pop.deactivate_provider("owner.a", true));
    assert(g_restores == 0 && delayed_pop.close_pending_for_test());
    assert(g_popup_count == 2 && g_popup_top == 0x9999);
    g_popup_count = 1;
    g_popup_top = 0x1234;
    assert(delayed_pop.pump(true));
    assert(g_restores == 0 && delayed_pop.close_pending_for_test());
    assert(delayed_pop.pump(true));
    assert(g_restores == 1 && !delayed_pop.close_pending_for_test());

    reset_fakes();
    auto strict_snapshot = make_host();
    assert(strict_snapshot.open(kPanelA, true));
    g_delay_pop = true;
    assert(strict_snapshot.close("panel.a", true));
    assert(strict_snapshot.pump(true));
    assert(g_restores == 0 && strict_snapshot.close_pending_for_test());
    g_popup_count = 1;
    g_popup_top = 0x1234;
    assert(strict_snapshot.pump(true));
    assert(g_restores == 0 && strict_snapshot.close_pending_for_test());
    assert(strict_snapshot.pump(true));
    assert(g_restores == 1 && !strict_snapshot.close_pending_for_test());

    reset_fakes();
    auto deactivate_after_close = make_host();
    assert(deactivate_after_close.open(kPanelA, true));
    assert(deactivate_after_close.close("panel.a", true));
    assert(!deactivate_after_close.deactivate_provider("owner.a", true));
    assert(deactivate_after_close.pump(true));
    assert(deactivate_after_close.pump(true));
    assert(deactivate_after_close.pump(true));
    assert(g_close_a == 1 && g_restores == 1);

    reset_fakes();
    auto deactivate_during_callback = make_host();
    assert(deactivate_during_callback.open(kPanelA, true));
    g_deactivate_during_event = &deactivate_during_callback;
    assert(deactivate_during_callback.dispatchers_for_test()->event(0, 0, 0) == 41);
    assert(g_close_a == 1 && g_restores == 0);
    assert(deactivate_during_callback.close_pending_for_test());
    assert(deactivate_during_callback.pump(true));
    assert(deactivate_during_callback.pump(true));
    assert(deactivate_during_callback.pump(true));
    assert(g_restores == 1);

    reset_fakes();
    auto deactivate_host = make_host();
    assert(deactivate_host.open(kPanelA, true));
    deactivate_host.deactivate(true);
    deactivate_host.deactivate(true);
    assert(deactivate_host.pump(true));
    assert(deactivate_host.pump(true)); assert(deactivate_host.pump(true));
    assert(!deactivate_host.active_for_test() && g_close_a == 1 && g_restores == 1);

    reset_fakes();
    auto deactivate_custom_ui = make_host();
    assert(deactivate_custom_ui.open(kPanelA, true));
    assert(deactivate_custom_ui.open(kPanelB, true));
    const int process_calls_before_deactivate = g_process_calls;
    deactivate_custom_ui.deactivate(true);
    assert(deactivate_custom_ui.pump(true));
    assert(g_process_calls == process_calls_before_deactivate + 1);
    assert(g_last_process == 3 && g_popup_count == 1 && g_popup_top == 0x1234);
    assert(deactivate_custom_ui.pump(true));
    assert(deactivate_custom_ui.pump(true));
    assert(g_close_a == 1 && g_close_b == 1 && g_restores == 1);

    reset_fakes();
    auto synchronous = make_host();
    g_sync_enter_on_push = true;
    assert(synchronous.open(kPanelA, true));
    assert(g_slots[0] == 1);

    reset_fakes();
    auto failed = make_host();
    g_acquire_ok = false;
    assert(!failed.open(kPanelA, true));
    assert(g_open_a == 0 && g_restores == 0 && !failed.active_for_test());

    reset_fakes();
    auto ui_failure = make_host();
    const PanelProviderSpec failed_ui{"panel.fail", "owner.fail", fail_open, close_a, active_a,
                                      reason, &kCallbacksA};
    assert(!ui_failure.open(failed_ui, true));
    assert(!ui_failure.active_for_test() && g_restores == 0 && g_close_a == 0);
    assert(ui_failure.pump(true)); assert(ui_failure.pump(true)); assert(ui_failure.pump(true));
    assert(g_restores == 1);

    reset_fakes();
    auto empty = make_host();
    const PanelProviderSpec no_callbacks{"panel.empty", "owner.empty", open_a, close_a, active_a, reason, nullptr};
    assert(empty.open(no_callbacks, true));
    assert(g_resolves == 0 && g_acquires == 0 && g_process_calls == 0);
    empty.dispatchers_for_test()->enter();
    assert(g_slots[0] == 0);
    empty.deactivate(true);
    assert(empty.pump(true));
    assert(g_restores == 0 && !empty.active_for_test());
}
