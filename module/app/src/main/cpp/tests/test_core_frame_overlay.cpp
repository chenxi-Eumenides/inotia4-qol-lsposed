// V1 native confirmation on a custom panel: the panel frame calls UIPopupMsg_Draw once,
// enabling the modal gate without changing V1 token/close semantics.
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <stdexcept>

#include "contracts/panels.h"
#include "contracts/ui.h"
#include "contracts/ui_confirmation.h"
#include "plugins/core.custom_ui/panels/frame_overlay.h"
#include "plugins/core.custom_ui/panels/host.h"
#include "plugins/core.custom_ui/ui_confirmation.h"

using qol::contracts::PanelCallbacks;
using qol::contracts::PanelProviderSpec;
using qol::contracts::QolPopupStateEntry;
using qol::contracts::QolPopupStateHooks;
using qol::contracts::QolUiAdapter;
using qol::features::core_ui::FrameOverlay;
using qol::features::core_ui::PopupHost;
using qol::features::core_ui::PopupHostHooks;

namespace {

int g_checks = 0;
int g_failures = 0;

#define CHECK(condition)                                                        \
    do {                                                                         \
        ++g_checks;                                                              \
        if (!(condition)) {                                                     \
            ++g_failures;                                                       \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition); \
        }                                                                        \
    } while (false)

FrameOverlay g_overlay;
QolUiAdapter g_ui{};
qol::contracts::QolHostSdk g_sdk{};
qol::contracts::QolGameHost g_game{};
PopupHost* g_host = nullptr;
uint32_t g_popup_count = 1;
uintptr_t g_popup_top = 0x111;
bool g_panel_active = false;
bool g_popup_active = false;
qol::contracts::QolUiPopupMsgCallbackFn g_ok = nullptr;
qol::contracts::QolUiPopupMsgCallbackFn g_cancel = nullptr;
bool g_call_end_frame = true;
bool g_draw_succeeds = true;
bool g_draw_throws = false;
bool g_fail_unregister_once = false;
int g_popup_draw_calls = 0;
int g_overlay_draw_calls = 0;
int g_end_frame_calls = 0;
int g_scope_begins = 0;
int g_scope_ends = 0;
int g_f3_calls = 0;
int g_cancel_callbacks = 0;
int64_t g_game_frame = 7;

bool fake_overlay_draw(void*, QolUiAdapter*, uint64_t) {
    ++g_overlay_draw_calls;
    return true;
}

QolResult fake_begin_overlay_scope(uint64_t token, const void* provider, uint64_t generation) {
    ++g_scope_begins;
    return g_overlay.begin_scope(token, provider, generation)
        ? qol::contracts::ok()
        : qol::contracts::error(kQolErrConflict, "scope", 5);
}

bool host_begin_overlay_scope(void*, uint64_t token, const void* provider, uint64_t generation) {
    ++g_scope_begins;
    return g_overlay.begin_scope(token, provider, generation);
}

void host_end_overlay_scope(void*) {
    ++g_scope_ends;
    g_overlay.end_scope();
}

QolResult fake_end_overlay_scope() {
    ++g_scope_ends;
    g_overlay.end_scope();
    return qol::contracts::ok();
}

QolResult fake_set_frame_overlay(qol::contracts::QolUiFrameOverlayDrawFn draw, void* user) {
    if (draw == nullptr && g_fail_unregister_once) {
        g_fail_unregister_once = false;
        return qol::contracts::error(kQolErrNotReady, "unregister", 9);
    }
    return g_overlay.set_draw(draw, user) ? qol::contracts::ok()
                                          : qol::contracts::error(kQolErrConflict, "owner", 5);
}

int32_t fake_main_thread() { return 1; }
int64_t fake_frame_count() { return g_game_frame; }

QolResult fake_popup_active(uint8_t* out) {
    if (out == nullptr) return qol::contracts::error(kQolErrInvalidArgument, "out", 3);
    *out = g_popup_active ? 1 : 0;
    return qol::contracts::ok();
}

QolResult fake_popup_callbacks(qol::contracts::QolUiPopupMsgCallbackFn* out_ok,
                               qol::contracts::QolUiPopupMsgCallbackFn* out_cancel) {
    if (out_ok == nullptr || out_cancel == nullptr) {
        return qol::contracts::error(kQolErrInvalidArgument, "out", 3);
    }
    *out_ok = g_ok;
    *out_cancel = g_cancel;
    return qol::contracts::ok();
}

QolResult fake_popup_replace(uint8_t is_ok, qol::contracts::QolUiPopupMsgCallbackFn expected,
                             qol::contracts::QolUiPopupMsgCallbackFn replacement, uint8_t* out) {
    if (out == nullptr) return qol::contracts::error(kQolErrInvalidArgument, "out", 3);
    auto& slot = is_ok != 0 ? g_ok : g_cancel;
    *out = slot == expected ? 1 : 0;
    if (*out != 0) slot = replacement;
    return qol::contracts::ok();
}

QolResult fake_popup_create(uint32_t, uint32_t, uint32_t,
                            qol::contracts::QolUiPopupMsgCallbackFn ok,
                            qol::contracts::QolUiPopupMsgCallbackFn cancel, uint32_t) {
    if (g_popup_active) return qol::contracts::error(kQolErrBusy, "busy", 4);
    g_popup_active = true;
    g_ok = ok;
    g_cancel = cancel;
    return qol::contracts::ok();
}

QolResult fake_popup_close(qol::contracts::QolUiPopupMsgCallbackFn,
                           qol::contracts::QolUiPopupMsgCallbackFn, uint8_t* out_closed) {
    if (out_closed == nullptr) return qol::contracts::error(kQolErrInvalidArgument, "out", 3);
    *out_closed = 1;
    g_popup_active = false;
    g_ok = nullptr;
    g_cancel = nullptr;
    return qol::contracts::ok();
}

QolResult fake_call_popup_draw() {
    ++g_popup_draw_calls;
    if (!g_popup_active) return qol::contracts::error(kQolErrNotReady, "inactive", 8);
    if (!g_draw_succeeds) return qol::contracts::error(kQolErrNotReady, "draw", 4);
    if (g_draw_throws) throw std::runtime_error("popup draw failure");
    return qol::contracts::ok();
}

QolResult fake_end_frame() {
    (void)g_overlay.draw_once(&g_ui);
    ++g_end_frame_calls;
    return qol::contracts::ok();
}

bool fake_resolve(void*, const char*, QolPopupStateEntry* out) {
    static uint8_t popup_slot[64]{};
    if (out == nullptr) return false;
    out->slot = popup_slot;
    out->state_id = 9;
    return true;
}

bool fake_acquire(void*, QolPopupStateEntry*, const QolPopupStateHooks*) { return true; }
bool fake_restore(void*, QolPopupStateEntry* entry) {
    if (entry != nullptr) entry->slot = nullptr;
    return true;
}

bool fake_set_process_info(void*, int32_t process, int32_t) {
    if (process == 1) {
        ++g_popup_count;
        g_popup_top = 0x222;
    } else if (process == 3) {
        if (g_popup_count > 0) --g_popup_count;
        g_popup_top = 0x111;
    }
    return true;
}

bool fake_snapshot(void*, uint32_t* count, uintptr_t* top) {
    if (count == nullptr || top == nullptr) return false;
    *count = g_popup_count;
    *top = g_popup_top;
    return true;
}

void fake_log(void*, int32_t, const char*) {}
void fake_save_lcd(void*) {}

QolResult fake_require(const char*, QolVersionRange, const void**) {
    return qol::contracts::error(kQolErrNotFound, "missing", 7);
}

bool panel_open() { g_panel_active = true; return true; }
void panel_close() { g_panel_active = false; }
bool panel_active() { return g_panel_active; }
const char* panel_reason() { return nullptr; }
void panel_process() {
    if (g_call_end_frame) {
        (void)g_ui.end_frame();
        (void)g_ui.end_frame();
    }
}
void panel_f3() { ++g_f3_calls; }
void panel_f4() {}
uint32_t panel_event(uint64_t, uint64_t, uint64_t) { return 7; }

const PanelCallbacks kCallbacks{&panel_process, &panel_process, &panel_f3, &panel_f4,
                                &panel_event};
const PanelProviderSpec kPanel{"panel", "test", &panel_open, &panel_close, &panel_active,
                               &panel_reason, &kCallbacks};

void confirmation_cancelled(void*) { ++g_cancel_callbacks; }
void confirmation_confirmed(void*) {}

QolResult open_confirmation(uint64_t* token) {
    const auto* api = qol::features::core_ui::ui_confirmation_api();
    const qol::contracts::UiConfirmationSpec spec{9, 0, 2, 0,
        &confirmation_confirmed, &confirmation_cancelled, nullptr};
    return api->open(&spec, token);
}

void setup_ui() {
    g_ui.size = sizeof(g_ui);
    g_ui.major = qol::contracts::kUiAdapterMajor;
    g_ui.minor = qol::contracts::kUiAdapterMinor;
    g_ui.is_main_thread = &fake_main_thread;
    g_ui.end_frame = &fake_end_frame;
    g_ui.popup_msg_active = &fake_popup_active;
    g_ui.popup_msg_get_callbacks = &fake_popup_callbacks;
    g_ui.popup_msg_replace_callback_if = &fake_popup_replace;
    g_ui.popup_msg_create_yesno_from_textdata = &fake_popup_create;
    g_ui.popup_msg_close_if_callbacks = &fake_popup_close;
    g_ui.call_ui_popup_msg_draw = &fake_call_popup_draw;
    g_ui.begin_overlay_scope = &fake_begin_overlay_scope;
    g_ui.end_overlay_scope = &fake_end_overlay_scope;
    g_ui.set_frame_overlay = &fake_set_frame_overlay;

    g_sdk.abi = {sizeof(g_sdk), qol::contracts::kHostSdkMajor, qol::contracts::kHostSdkMinor};
    g_sdk.capabilities.require = &fake_require;
    g_game.abi = {sizeof(g_game), qol::contracts::kGameHostMajor, qol::contracts::kGameHostMinor};
    g_game.thread.frame_count = &fake_frame_count;
}

}  // namespace

int main() {
    setup_ui();
    PopupHostHooks hooks{nullptr, &fake_resolve, &fake_acquire, &fake_restore,
                         &fake_set_process_info, &fake_snapshot, &fake_log,
                         &host_begin_overlay_scope, &host_end_overlay_scope, &fake_save_lcd};
    PopupHost host("test-popup", hooks);
    g_host = &host;
    CHECK(host.open(kPanel, true));

    CHECK(qol::features::core_ui::ui_confirmation_bind(&g_ui, &g_game, &g_sdk));
    CHECK(qol::features::core_ui::ui_confirmation_set_installed(true));
    CHECK(qol::features::core_ui::ui_confirmation_register_builtin_frame_overlay());

    // V1 + panel: one native draw per frame, modal gate enabled after that draw.
    uint64_t token = 0;
    CHECK(qol::contracts::succeeded(open_confirmation(&token)) && token != 0 && g_popup_active);
    const int draw_before = g_popup_draw_calls;
    host.dispatch_process();
    CHECK(g_popup_draw_calls == draw_before + 1);
    CHECK(qol::features::core_ui::ui_confirmation_modal_input_active());
    CHECK(g_f3_calls == 0);

    // Touch/f4 are consumed without routing into panel hit-testing; F3 is consumed for Return.
    CHECK(host.dispatch_event(0x17, 1, 0) == 1);
    CHECK(host.dispatch_event(0x80000001ULL, 1, 0) == 1);
    CHECK(host.dispatch_event(0x18, 1, 0) == 1);
    host.dispatch_f4();
    host.dispatch_f3();
    CHECK(g_f3_calls == 0 && token != 0);

    // Draw failure before the first successful frame keeps V1 semantics: token remains owned,
    // the gate stays off, and close still works.
    CHECK(qol::contracts::succeeded(qol::features::core_ui::ui_confirmation_api()->close(token)));
    token = 0;
    CHECK(qol::contracts::succeeded(open_confirmation(&token)));
    g_draw_succeeds = false;
    host.dispatch_process();
    CHECK(!qol::features::core_ui::ui_confirmation_modal_input_active());
    CHECK(token != 0);
    CHECK(qol::contracts::succeeded(qol::features::core_ui::ui_confirmation_api()->close(token)));

    // bOn == 0: the next frame ends the scope and drops the gate; token/close stays valid.
    token = 0;
    CHECK(qol::contracts::succeeded(open_confirmation(&token)));
    g_popup_active = false;
    host.dispatch_process();
    CHECK(!qol::features::core_ui::ui_confirmation_modal_input_active());
    CHECK(qol::contracts::succeeded(qol::features::core_ui::ui_confirmation_api()->close(token)));

    // Re-enable after a failed unregister still restores the built-in draw registration.
    g_fail_unregister_once = true;
    CHECK(!qol::features::core_ui::ui_confirmation_set_installed(false));
    CHECK(qol::features::core_ui::ui_confirmation_set_installed(true));
    CHECK(qol::features::core_ui::ui_confirmation_register_builtin_frame_overlay());

    // Native-page V1 (no custom panel host) never composites or gates.
    CHECK(host.close("panel", true));
    token = 0;
    CHECK(qol::contracts::succeeded(open_confirmation(&token)));
    CHECK(!qol::features::core_ui::ui_confirmation_modal_input_active());
    host.dispatch_f3();
    CHECK(qol::contracts::succeeded(qol::features::core_ui::ui_confirmation_api()->close(token)));

    qol::features::core_ui::ui_confirmation_unbind();
    std::printf("core_frame_overlay_tests: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
