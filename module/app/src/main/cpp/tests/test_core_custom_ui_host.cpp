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
bool g_a_active = false;
bool g_b_active = false;
int g_open_a = 0;
int g_open_b = 0;
int g_close_a = 0;
int g_close_b = 0;
int g_slots[5] = {};
uint32_t g_event_result = 0;
const QolPopupStateHooks* g_acquired_hooks = nullptr;
bool g_sync_enter_on_push = false;

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

bool set_process_info(void*, int32_t process, int32_t state_id) {
    ++g_process_calls;
    g_last_process = process;
    g_last_state_id = state_id;
    if (process == 1 && g_sync_enter_on_push && g_acquired_hooks != nullptr) {
        g_acquired_hooks->enter();
    }
    return true;
}

void a_enter() { ++g_slots[0]; }
void a_process() { ++g_slots[1]; }
void a_f3() { ++g_slots[2]; }
void a_f4() { ++g_slots[3]; }
uint32_t a_event(uint64_t, uint64_t, uint64_t) { ++g_slots[4]; return 41; }
void b_enter() { g_slots[0] += 10; }
void b_process() { g_slots[1] += 10; }
void b_f3() { g_slots[2] += 10; }
void b_f4() { g_slots[3] += 10; }
uint32_t b_event(uint64_t, uint64_t, uint64_t) { g_slots[4] += 10; return 42; }

bool open_a() { ++g_open_a; g_a_active = true; return true; }
bool fail_open() { ++g_open_a; return false; }
bool open_b() { ++g_open_b; g_b_active = true; return true; }
void close_a() { ++g_close_a; g_a_active = false; }
void close_b() { ++g_close_b; g_b_active = false; }
bool active_a() { return g_a_active; }
bool active_b() { return g_b_active; }
const char* reason() { return nullptr; }

const PanelCallbacks kCallbacksA{a_enter, a_process, a_f3, a_f4, a_event};
const PanelCallbacks kCallbacksB{b_enter, b_process, b_f3, b_f4, b_event};
const PanelProviderSpec kPanelA{"panel.a", "owner.a", open_a, close_a, active_a, reason, &kCallbacksA};
const PanelProviderSpec kPanelB{"panel.b", "owner.b", open_b, close_b, active_b, reason, &kCallbacksB};

void reset_fakes() {
    g_resolves = g_acquires = g_restores = 0;
    g_process_calls = 0;
    g_last_process = g_last_state_id = -1;
    g_acquired_hooks = nullptr;
    g_sync_enter_on_push = false;
    g_acquire_ok = g_resolve_ok = true;
    g_a_active = g_b_active = false;
    g_open_a = g_open_b = g_close_a = g_close_b = 0;
    std::memset(g_slots, 0, sizeof(g_slots));
}

PopupHost make_host() {
    return PopupHost("TEST_POPUP", {nullptr, resolve_popup, acquire_popup, restore_popup,
                                     set_process_info});
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
    assert(g_close_a == 1 && g_open_b == 1 && g_acquires == 1);
    dispatch->enter();
    assert(g_slots[0] == 11);
    assert(host.close("panel.b", true));
    assert(g_close_b == 1 && g_open_a == 2 && host.current_panel_for_test() == "panel.a");

    assert(host.close("panel.a", true));
    assert(!host.close("panel.a", true));
    assert(g_restores == 0 && host.close_pending_for_test());
    assert(g_last_process == 3);
    assert(host.pump(true));
    assert(g_restores == 1 && !host.close_pending_for_test());

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
    assert(!ui_failure.active_for_test() && g_restores == 1 && g_close_a == 0);

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
