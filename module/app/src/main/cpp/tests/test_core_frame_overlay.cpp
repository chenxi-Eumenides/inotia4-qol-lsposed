#include <cstdint>
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>
#include <stdexcept>
#include <vector>

#include "contracts/panels.h"
#include "contracts/ui.h"
#include "contracts/ui_confirmation.h"
#include "plugins/core.custom_ui/panels/frame_overlay.h"
#include "plugins/core.custom_ui/panels/host.h"
#include "plugins/core.custom_ui/ui_confirmation.h"
#include "plugins/core.custom_ui/ui_confirmation_overlay.h"

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
PopupHost* g_current_host = nullptr;
QolUiAdapter g_ui{};
qol::contracts::QolHostSdk g_sdk{};
qol::contracts::QolGameHost g_game{};
uint32_t g_popup_count = 1;
uintptr_t g_popup_top = 0x111;
bool g_parent_active = false;
bool g_child_active = false;
bool g_popup_active = false;
qol::contracts::QolUiPopupMsgCallbackFn g_ok = nullptr;
qol::contracts::QolUiPopupMsgCallbackFn g_cancel = nullptr;
int g_scope_begins = 0;
int g_scope_ends = 0;
int g_draw_calls = 0;
int g_end_frame_calls = 0;
int g_order = 0;
int g_draw_order = 0;
int g_native_end_order = 0;
int g_draw_calls_after_mismatches = 0;
int g_f3_calls = 0;
int g_f4_calls = 0;
int g_event_calls = 0;
bool g_call_end_frame = false;
bool g_call_end_after_mismatches = false;
bool g_check_mismatches = false;
bool g_process_throws = false;
bool g_draw_succeeds = true;
bool g_draw_throws = false;
bool g_mismatch_results[3]{};
bool g_use_stale_scope = false;
bool g_trigger_nested_process = false;
bool g_inside_nested_process = false;
bool g_open_confirmation_in_nested = false;
uint64_t g_nested_token = 0;
bool g_component_fail_fill = false;
bool g_fail_overlay_unregister_once = false;
int g_component_draw_calls = 0;
int g_cancel_callbacks = 0;
int g_confirm_callbacks = 0;
int32_t g_screen_inset_w = 0;
int32_t g_screen_inset_h = 0;
std::string g_last_component_text;
std::vector<std::string> g_component_texts;
int64_t g_fake_game_frame = 7;
uint64_t g_ui_state_sequence = 0;
uint64_t g_ui_state_generation = 0;
int g_ui_state_publish_calls = 0;
int g_ui_state_clear_calls = 0;
bool g_ui_state_dialog_active = false;
qol::contracts::UiCustomDialogSnapshotV2 g_last_ui_dialog{};
FrameOverlay::Scope g_last_scope{};
FrameOverlay::Scope g_stale_scope{};

bool fake_draw(void* user, QolUiAdapter* ui, uint64_t token) {    ++g_draw_calls;
    g_draw_order = ++g_order;
    if (user != &g_draw_calls || ui != &g_ui || token == 0) return false;
    if (g_draw_throws) throw std::runtime_error("fake overlay draw failure");
    return g_draw_succeeds;
}

QolResult fake_screen_inset_width(int32_t* out) {
    if (out == nullptr) return qol::contracts::error(kQolErrInvalidArgument, "out", 3);
    *out = g_screen_inset_w;
    return qol::contracts::ok();
}

QolResult fake_screen_inset_height(int32_t* out) {
    if (out == nullptr) return qol::contracts::error(kQolErrInvalidArgument, "out", 3);
    *out = g_screen_inset_h;
    return qol::contracts::ok();
}

QolResult fake_fill_rect(qol::contracts::UiRect, uint32_t, uint32_t) {
    ++g_component_draw_calls;
    return g_component_fail_fill
        ? qol::contracts::error(kQolErrNotReady, "fake fill failed", 16)
        : qol::contracts::ok();
}

QolResult fake_draw_text(const char* text, int32_t, int32_t, int32_t, int32_t) {
    ++g_component_draw_calls;
    g_last_component_text = text == nullptr ? "" : text;
    g_component_texts.push_back(g_last_component_text);
    return qol::contracts::ok();
}

QolResult fake_set_font_color(uint32_t) { return qol::contracts::ok(); }

QolResult fake_touch_coordinates(uint64_t param, int64_t* out_x, int64_t* out_y) {
    if (param == 0 || out_x == nullptr || out_y == nullptr) {
        return qol::contracts::error(kQolErrInvalidArgument, "point", 5);
    }
    const auto* point = reinterpret_cast<const int64_t*>(static_cast<uintptr_t>(param));
    *out_x = point[0];
    *out_y = point[1];
    return qol::contracts::ok();
}

QolResult fake_set_frame_overlay(qol::contracts::QolUiFrameOverlayDrawFn draw, void* user) {
    if (draw == nullptr && g_fail_overlay_unregister_once) {
        g_fail_overlay_unregister_once = false;
        return qol::contracts::error(kQolErrNotReady, "fake unregister failed", 21);
    }
    if (!g_overlay.set_draw(draw, user)) {
        static constexpr char message[] = "overlay owner conflict";
        return qol::contracts::error(kQolErrConflict, message, sizeof(message) - 1);
    }
    return qol::contracts::ok();
}

QolResult fake_end_frame() {
    (void)g_overlay.draw_once(&g_ui);
    ++g_end_frame_calls;
    g_native_end_order = ++g_order;
    return qol::contracts::ok();
}

int32_t fake_main_thread() { return 1; }

QolResult fake_popup_active(uint8_t* out_active) {
    if (out_active == nullptr) return qol::contracts::error(kQolErrInvalidArgument, "out", 3);
    *out_active = g_popup_active ? 1 : 0;
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
                              qol::contracts::QolUiPopupMsgCallbackFn replacement,
                              uint8_t* out_replaced) {
    if (out_replaced == nullptr) return qol::contracts::error(kQolErrInvalidArgument, "out", 3);
    auto& slot = is_ok != 0 ? g_ok : g_cancel;
    *out_replaced = slot == expected ? 1 : 0;
    if (*out_replaced != 0) slot = replacement;
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

QolResult fake_popup_close(qol::contracts::QolUiPopupMsgCallbackFn expected_ok,
                           qol::contracts::QolUiPopupMsgCallbackFn expected_cancel,
                           uint8_t* out_closed) {
    if (out_closed == nullptr) return qol::contracts::error(kQolErrInvalidArgument, "out", 3);
    *out_closed = 0;
    if (!g_popup_active || g_ok != expected_ok || g_cancel != expected_cancel) {
        return qol::contracts::error(kQolErrConflict, "owner", 5);
    }
    auto cancel = g_cancel;
    g_popup_active = false;
    *out_closed = 1;
    if (cancel != nullptr) cancel(nullptr);
    return qol::contracts::ok();
}

QolResult fake_publish_custom_dialog_v2(
    const qol::contracts::UiCustomDialogSnapshotV2* snapshot) {
    if (snapshot == nullptr || snapshot->size < sizeof(*snapshot)) {
        return qol::contracts::error(kQolErrInvalidArgument, "snapshot", 8);
    }
    if (snapshot->sequence <= g_ui_state_sequence ||
        snapshot->generation <= g_ui_state_generation) {
        return qol::contracts::error(kQolErrConflict, "stale", 5);
    }
    g_ui_state_sequence = snapshot->sequence;
    g_ui_state_generation = snapshot->generation;
    g_last_ui_dialog = *snapshot;
    g_ui_state_dialog_active = true;
    ++g_ui_state_publish_calls;
    return qol::contracts::ok();
}

QolResult fake_clear_custom_dialog(uint64_t generation, uint64_t sequence) {
    if (sequence <= g_ui_state_sequence) {
        return qol::contracts::error(kQolErrConflict, "stale", 5);
    }
    g_ui_state_sequence = sequence;
    if (generation == g_ui_state_generation) g_ui_state_dialog_active = false;
    ++g_ui_state_clear_calls;
    return qol::contracts::ok();
}

QolResult fake_publish_custom_panels(const qol::contracts::UiCustomPanelsSnapshot*) {
    return qol::contracts::ok();
}

const qol::contracts::UiStateApi kFakeUiStateApi{
    &fake_publish_custom_panels, nullptr, &fake_clear_custom_dialog, nullptr,
    &fake_publish_custom_dialog_v2};

QolResult fake_require_ui_state(const char* id, QolVersionRange range, const void** out) {
    if (id == nullptr || out == nullptr ||
        std::strcmp(id, qol::contracts::kUiStateCapabilityId) != 0 ||
        range.major != qol::contracts::kUiStateVersionMajor ||
        range.min_minor > qol::contracts::kUiStateVersionMinor) {
        return qol::contracts::error(kQolErrNotFound, "missing", 7);
    }
    *out = &kFakeUiStateApi;
    return qol::contracts::ok();
}

int64_t fake_frame_count() { return g_fake_game_frame; }

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

bool fake_begin_scope(void*, uint64_t token, const void* provider, uint64_t generation) {
    ++g_scope_begins;
    const bool armed = g_overlay.begin_scope(token, provider, generation);
    if (armed) g_last_scope = g_overlay.current_scope();
    return armed;
}

void fake_end_scope(void*) {
    ++g_scope_ends;
    g_overlay.end_scope();
}

QolResult fake_begin_overlay_scope(uint64_t token, const void* provider, uint64_t generation) {
    return fake_begin_scope(nullptr, token, provider, generation)
        ? qol::contracts::ok()
        : qol::contracts::error(kQolErrConflict, "scope", 5);
}

QolResult fake_end_overlay_scope() {
    fake_end_scope(nullptr);
    return qol::contracts::ok();
}

bool parent_open() { g_parent_active = true; return true; }
void parent_close() { g_parent_active = false; }
bool parent_active() { return g_parent_active; }
bool child_open() { g_child_active = true; return true; }
void child_close() { g_child_active = false; }
bool child_active() { return g_child_active; }
const char* no_block_reason() { return nullptr; }

QolResult open_custom_confirmation(uint64_t* token, uint32_t price = 0,
                                   const char* message = "确认测试？",
                                   const char* confirm_label = "确认",
                                   const char* cancel_label = "取消",
                                   const char* price_unit = "金币");

void panel_process() {
    if (g_trigger_nested_process && !g_inside_nested_process && g_current_host != nullptr) {
        g_trigger_nested_process = false;
        g_inside_nested_process = true;
        if (g_open_confirmation_in_nested) {
            g_open_confirmation_in_nested = false;
            (void)open_custom_confirmation(&g_nested_token);
        }
        g_current_host->dispatch_process();
        g_inside_nested_process = false;
    }
    if (g_check_mismatches) {
        FrameOverlay::Scope wrong = g_overlay.current_scope();
        ++wrong.token;
        g_mismatch_results[0] = g_overlay.draw_once_for_scope(&g_ui, wrong);
        wrong = g_overlay.current_scope();
        wrong.provider = reinterpret_cast<const void*>(uintptr_t{0x55});
        g_mismatch_results[1] = g_overlay.draw_once_for_scope(&g_ui, wrong);
        wrong = g_overlay.current_scope();
        ++wrong.generation;
        g_mismatch_results[2] = g_overlay.draw_once_for_scope(&g_ui, wrong);
        if (g_use_stale_scope) {
            g_mismatch_results[2] = g_overlay.draw_once_for_scope(&g_ui, g_stale_scope);
        }
        g_draw_calls_after_mismatches = g_draw_calls;
    }
    if (g_call_end_frame || g_call_end_after_mismatches) {
        (void)g_ui.end_frame();
        (void)g_ui.end_frame();
    }
    if (g_process_throws) throw std::runtime_error("fake provider process failure");
}

void panel_f3() { ++g_f3_calls; }
void panel_f4() { ++g_f4_calls; }
uint32_t panel_event(uint64_t, uint64_t, uint64_t) { ++g_event_calls; return 7; }
void confirmation_cancelled(void*) { ++g_cancel_callbacks; }
void confirmation_confirmed(void*) { ++g_confirm_callbacks; }

const PanelCallbacks kCallbacks{&panel_process, &panel_process, &panel_f3, &panel_f4,
                                &panel_event};
const PanelProviderSpec kParent{"parent", "test", &parent_open, &parent_close,
                                &parent_active, &no_block_reason, &kCallbacks};
const PanelProviderSpec kChild{"same", "test", &child_open, &child_close,
                               &child_active, &no_block_reason, &kCallbacks};
const PanelProviderSpec kNoProcess{"noproc", "test", &child_open, &child_close,
                                   &child_active, &no_block_reason, nullptr};

QolResult open_native_confirmation(uint64_t* token, uint32_t price = 0) {
    const auto* api = qol::features::core_ui::ui_confirmation_api();
    const qol::contracts::UiConfirmationSpec spec{9, 0, 0, price,
        &confirmation_confirmed, &confirmation_cancelled, nullptr};
    return api->open(&spec, token);
}

QolResult open_custom_confirmation(uint64_t* token, uint32_t price,
                                   const char* message, const char* confirm_label,
                                   const char* cancel_label, const char* price_unit) {
    const auto* api = qol::features::core_ui::ui_confirmation_api();
    qol::contracts::UiConfirmationSpecV2 spec{};
    spec.size = sizeof(spec);
    spec.text_id = 9;
    spec.display_type = 0;
    spec.format_arg = 2;
    spec.price = price;
    spec.on_confirm = &confirmation_confirmed;
    spec.on_cancel = &confirmation_cancelled;
    spec.message[0] = '\0';
    std::snprintf(spec.message, sizeof(spec.message), "%s", message);
    std::snprintf(spec.confirm_label, sizeof(spec.confirm_label), "%s", confirm_label);
    std::snprintf(spec.cancel_label, sizeof(spec.cancel_label), "%s", cancel_label);
    std::snprintf(spec.price_unit, sizeof(spec.price_unit), "%s", price_unit);
    return api->open_v2(&spec, token);
}

bool setup_host(PopupHost* host) {
    return host->open(kParent, true) && host->open(kChild, true);
}

}  // namespace

int main() {
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
    g_ui.set_frame_overlay = &fake_set_frame_overlay;
    g_ui.begin_overlay_scope = &fake_begin_overlay_scope;
    g_ui.end_overlay_scope = &fake_end_overlay_scope;
    g_ui.screen_inset_width = &fake_screen_inset_width;
    g_ui.screen_inset_height = &fake_screen_inset_height;
    g_ui.fill_rect_alpha = &fake_fill_rect;
    g_ui.draw_string_with_font = &fake_draw_text;
    g_ui.set_font_color = &fake_set_font_color;
    g_ui.touch_coordinates = &fake_touch_coordinates;

    g_sdk.abi = {sizeof(g_sdk), qol::contracts::kHostSdkMajor, qol::contracts::kHostSdkMinor};
    g_sdk.capabilities.require = &fake_require_ui_state;
    g_game.abi = {sizeof(g_game), qol::contracts::kGameHostMajor, qol::contracts::kGameHostMinor};
    g_game.thread.frame_count = &fake_frame_count;

    PopupHostHooks hooks{nullptr, &fake_resolve, &fake_acquire, &fake_restore,
                         &fake_set_process_info, &fake_snapshot, &fake_log,
                         &fake_begin_scope, &fake_end_scope};
    PopupHost host("test-popup", hooks);
    g_current_host = &host;
    CHECK(setup_host(&host));

    CHECK(qol::features::core_ui::ui_confirmation_bind(&g_ui, &g_game, &g_sdk));
    qol::features::core_ui::ui_confirmation_set_installed(true);
    CHECK(qol::features::core_ui::ui_confirmation_register_frame_overlay(&fake_draw,
                                                                          &g_draw_calls));
    uint64_t token = 0;
    CHECK(qol::contracts::succeeded(open_custom_confirmation(&token)));
    CHECK(token != 0);
    CHECK(g_popup_active == 0);  // Custom backend leaves UIPopupMsg inactive.
    CHECK(g_ui_state_dialog_active && g_last_ui_dialog.size == sizeof(g_last_ui_dialog));
    CHECK(std::strcmp(g_last_ui_dialog.body, "确认测试？") == 0);

    // The custom snapshot is modal from open: before the first draw it consumes panel input
    // (event/F4) without forwarding, while Return still cancels the token.
    CHECK(host.dispatch_event(0, 0, 0) == 1);
    CHECK(g_event_calls == 0);
    host.dispatch_f4();
    CHECK(g_f4_calls == 0);
    const int cancel_before_first_return = g_cancel_callbacks;
    host.dispatch_f3();
    CHECK(g_f3_calls == 0);
    CHECK(g_cancel_callbacks == cancel_before_first_return + 1);
    CHECK(!g_ui_state_dialog_active);
    host.dispatch_f3();
    CHECK(g_f3_calls == 1);

    // Provider process with no end_frame disarms and fail-closes the unseen custom modal.
    token = 0;
    CHECK(qol::contracts::succeeded(open_custom_confirmation(&token)));
    const int draw_before_failed_frame = g_draw_calls;
    const int begins_before_failed_frame = g_scope_begins;
    const int ends_before_failed_frame = g_scope_ends;
    const int cancels_before_failed_frame = g_cancel_callbacks;
    g_call_end_frame = false;
    host.dispatch_process();
    CHECK(g_scope_begins == begins_before_failed_frame + 1 &&
          g_scope_ends == ends_before_failed_frame + 1);
    CHECK(!g_overlay.scope_live());
    CHECK(g_draw_calls == draw_before_failed_frame);
    CHECK(!qol::features::core_ui::ui_confirmation_modal_input_active());
    CHECK(g_cancel_callbacks == cancels_before_failed_frame + 1 && !g_ui_state_dialog_active);
    host.dispatch_f3();
    CHECK(g_f3_calls == 2);

    // An exception escaping provider process still disarms and cancels the unseen modal.
    token = 0;
    CHECK(qol::contracts::succeeded(open_custom_confirmation(&token)));
    g_process_throws = true;
    bool process_exception_caught = false;
    const int begins_before_throw = g_scope_begins;
    const int ends_before_throw = g_scope_ends;
    const int cancels_before_throw = g_cancel_callbacks;
    try {
        host.dispatch_process();
    } catch (const std::runtime_error&) {
        process_exception_caught = true;
    }
    g_process_throws = false;
    CHECK(process_exception_caught);
    CHECK(g_scope_begins == begins_before_throw + 1 && g_scope_ends == ends_before_throw + 1);
    CHECK(!g_overlay.scope_live());
    CHECK(!qol::features::core_ui::ui_confirmation_modal_input_active());
    CHECK(g_cancel_callbacks == cancels_before_throw + 1 && !g_ui_state_dialog_active);
    host.dispatch_f3();
    CHECK(g_f3_calls == 3);

    // All scope identity fields must match; mismatches do not draw or consume the live scope.
    token = 0;
    CHECK(qol::contracts::succeeded(open_custom_confirmation(&token)));
    g_check_mismatches = true;
    g_call_end_after_mismatches = true;
    const int begins_before_mismatch = g_scope_begins;
    const int ends_before_mismatch = g_scope_ends;
    host.dispatch_process();
    CHECK(!g_mismatch_results[0] && !g_mismatch_results[1] && !g_mismatch_results[2]);
    CHECK(g_draw_calls_after_mismatches == draw_before_failed_frame);
    CHECK(g_draw_calls == draw_before_failed_frame + 1);
    CHECK(g_scope_begins == begins_before_mismatch + 1 &&
          g_scope_ends == ends_before_mismatch + 1);
    g_check_mismatches = false;
    g_call_end_after_mismatches = false;

    // Successful draw occurs before the native end_frame and repeated end_frame consumes once.
    g_draw_succeeds = true;
    g_call_end_frame = true;
    const int draw_before_success = g_draw_calls;
    const int frame_ends_before = g_end_frame_calls;
    const int begins_before_success = g_scope_begins;
    const int ends_before_success = g_scope_ends;
    host.dispatch_process();
    CHECK(g_draw_calls == draw_before_success + 1);
    CHECK(g_end_frame_calls == frame_ends_before + 2);
    CHECK(g_draw_order < g_native_end_order);
    CHECK(g_scope_begins == begins_before_success + 1 &&
          g_scope_ends == ends_before_success + 1);
    CHECK(!g_overlay.scope_live());
    CHECK(qol::features::core_ui::ui_confirmation_modal_input_active());

    // The nested process gets a blocked sentinel: its end_frame calls cannot consume the outer scope.
    g_trigger_nested_process = true;
    const int draws_before_nested = g_draw_calls;
    const int begins_before_nested = g_scope_begins;
    const int ends_before_nested = g_scope_ends;
    const int native_ends_before_nested = g_end_frame_calls;
    host.dispatch_process();
    CHECK(g_draw_calls == draws_before_nested + 1);
    CHECK(g_scope_begins == begins_before_nested + 2);
    CHECK(g_scope_ends == ends_before_nested + 2);
    CHECK(g_end_frame_calls == native_ends_before_nested + 4);
    CHECK(!g_overlay.scope_live());

    const int f3_before_gate = g_f3_calls;
    const int f4_before_gate = g_f4_calls;
    const int event_before_gate = g_event_calls;
    host.dispatch_f4();
    CHECK(host.dispatch_event(0, 0, 0) == 1);
    CHECK(g_f3_calls == f3_before_gate && g_f4_calls == f4_before_gate);
    CHECK(g_event_calls == event_before_gate);
    const int cancelled_before_return = g_cancel_callbacks;
    host.dispatch_f3();
    CHECK(g_f3_calls == f3_before_gate);
    CHECK(g_cancel_callbacks == cancelled_before_return + 1);
    CHECK(!qol::features::core_ui::ui_confirmation_modal_input_active());
    host.dispatch_f3();
    CHECK(g_f3_calls == f3_before_gate + 1);

    // A confirmation opened inside a nested dispatch belongs to the outer scope: the inner
    // cleanup must not cancel it, and the first outer frame draws before any cancel fallback.
    const int cancel_before_nested_open = g_cancel_callbacks;
    const int draw_before_nested_open = g_draw_calls;
    g_open_confirmation_in_nested = true;
    g_trigger_nested_process = true;
    host.dispatch_process();
    CHECK(g_nested_token != 0);
    CHECK(g_draw_calls == draw_before_nested_open + 1);
    CHECK(qol::features::core_ui::ui_confirmation_modal_input_active());
    CHECK(g_cancel_callbacks == cancel_before_nested_open);
    host.dispatch_f3();
    CHECK(g_cancel_callbacks == cancel_before_nested_open + 1);
    CHECK(!qol::features::core_ui::ui_confirmation_modal_input_active());
    g_nested_token = 0;

    // System sub-events must be consumed without dereferencing param as touch coordinates.
    CHECK(qol::contracts::succeeded(open_custom_confirmation(&token)));
    CHECK(host.dispatch_event(0x80000001ULL, 1, 0) == 1);
    CHECK(host.dispatch_event(0x99, 1, 0) == 1);
    host.dispatch_f3();
    CHECK(!qol::features::core_ui::ui_confirmation_modal_input_active());

    // A top provider without a process callback cannot open a render scope.
    CHECK(host.open(kNoProcess, true));
    token = 0;
    CHECK(!qol::contracts::succeeded(open_custom_confirmation(&token)));
    CHECK(token == 0);
    CHECK(host.close("noproc", true));

    // Reopening the same panel id gets a fresh generation; a captured old generation is stale.
    token = 0;
    CHECK(qol::contracts::succeeded(open_custom_confirmation(&token)));
    const int draws_before_reopen = g_draw_calls;
    host.dispatch_process();
    CHECK(g_draw_calls == draws_before_reopen + 1);
    const uint64_t old_generation = host.top_open_generation_for_test();
    const FrameOverlay::Scope old_scope = g_last_scope;
    CHECK(host.close("same", true));
    CHECK(host.open(kChild, true));
    const uint64_t new_generation = host.top_open_generation_for_test();
    CHECK(new_generation > old_generation);
    g_call_end_frame = false;
    g_check_mismatches = true;
    g_use_stale_scope = true;
    g_stale_scope = old_scope;
    host.dispatch_process();
    CHECK(!g_mismatch_results[2]);
    CHECK(g_draw_calls == draws_before_reopen + 1);
    CHECK(old_scope.generation == old_generation && old_scope.generation != new_generation);
    g_check_mismatches = false;
    g_use_stale_scope = false;

    // UIPopupMsg can invoke Cancel before PopupHost sees the same Return key. Its one-frame
    // suppression latch prevents that key from reaching the parent F3 callback.
    const int cancelled_before_native_return = g_cancel_callbacks;
    const int f3_before_native_return = g_f3_calls;
    CHECK(qol::contracts::succeeded(qol::features::core_ui::ui_confirmation_api()->close(token)));
    token = 0;
    CHECK(qol::contracts::succeeded(open_native_confirmation(&token)));
    auto native_cancel = g_cancel;
    g_popup_active = false;
    CHECK(native_cancel != nullptr);
    native_cancel(nullptr);
    CHECK(g_cancel_callbacks == cancelled_before_native_return + 1);
    CHECK(!qol::features::core_ui::ui_confirmation_modal_input_active());
    const int end_frames_before_cancel = g_end_frame_calls;
    (void)g_ui.end_frame();
    CHECK(g_end_frame_calls == end_frames_before_cancel + 1);
    host.dispatch_f3();
    CHECK(g_f3_calls == f3_before_native_return);
    ++g_fake_game_frame;
    host.dispatch_f3();
    CHECK(g_f3_calls == f3_before_native_return + 1);

    // A failed or throwing callback is consumed before invocation, so another end_frame cannot retry.
    token = 0;
    CHECK(qol::contracts::succeeded(open_custom_confirmation(&token)));
    g_draw_succeeds = false;
    g_call_end_frame = true;
    const int draw_before_failure = g_draw_calls;
    const int cancel_before_draw_failure = g_cancel_callbacks;
    host.dispatch_process();
    CHECK(g_draw_calls == draw_before_failure + 1);
    CHECK(!qol::features::core_ui::ui_confirmation_modal_input_active());
    CHECK(g_cancel_callbacks == cancel_before_draw_failure + 1);

    token = 0;
    CHECK(qol::contracts::succeeded(open_custom_confirmation(&token)));
    g_draw_throws = true;
    const int draw_before_throw = g_draw_calls;
    const int cancel_before_draw_throw = g_cancel_callbacks;
    host.dispatch_process();
    CHECK(g_draw_calls == draw_before_throw + 1);
    CHECK(!qol::features::core_ui::ui_confirmation_modal_input_active());
    CHECK(g_cancel_callbacks == cancel_before_draw_throw + 1);
    g_draw_throws = false;

    // Disable unregisters the host-owned callback before later end_frame calls.
    const int draws_before_unbind = g_draw_calls;
    (void)qol::features::core_ui::ui_confirmation_set_installed(false);
    CHECK(!g_overlay.scope_live());
    CHECK(g_overlay.begin_scope(91, &kChild, new_generation));
    (void)g_ui.end_frame();
    CHECK(g_draw_calls == draws_before_unbind);
    CHECK(g_overlay.set_draw(nullptr, &g_draw_calls));
    g_overlay.end_scope();
    qol::features::core_ui::ui_confirmation_unbind();

    // The bound built-in draw callback projects the active confirmation content to the designer renderer.
    CHECK(qol::features::core_ui::ui_confirmation_bind(&g_ui, &g_game, &g_sdk));
    qol::features::core_ui::ui_confirmation_set_installed(true);
    CHECK(qol::features::core_ui::ui_confirmation_register_builtin_frame_overlay());
    g_screen_inset_w = 0;
    g_screen_inset_h = 0;
    uint64_t overlay_token = 0;
    CHECK(qol::contracts::succeeded(open_custom_confirmation(
        &overlay_token, 125, "确认是否出售？", "出售", "返回", "金币")));
    const int component_draw_before = g_component_draw_calls;
    host.dispatch_process();
    CHECK(g_component_draw_calls > component_draw_before);
    CHECK(std::find(g_component_texts.begin(), g_component_texts.end(), "确认是否出售？") !=
          g_component_texts.end());
    CHECK(std::find(g_component_texts.begin(), g_component_texts.end(), "125 金币") !=
          g_component_texts.end());
    CHECK(std::find(g_component_texts.begin(), g_component_texts.end(), "出售") !=
          g_component_texts.end());
    CHECK(std::find(g_component_texts.begin(), g_component_texts.end(), "返回") !=
          g_component_texts.end());
    CHECK(qol::features::core_ui::ui_confirmation_modal_input_active());
    CHECK(g_ui_state_dialog_active &&
          std::strcmp(g_last_ui_dialog.confirm_label, "出售") == 0 &&
          std::strcmp(g_last_ui_dialog.cancel_label, "返回") == 0 &&
          g_last_ui_dialog.has_price == 1 && g_last_ui_dialog.price == 125);

    const qol::plugins::custom_ui::UiConfirmationOverlayContent hit_content{
        "确认是否出售？", "出售", "返回", true, 125, "金币"};
    qol::plugins::custom_ui::UiConfirmationOverlayLayout hit_layout{};
    CHECK(qol::plugins::custom_ui::ui_confirmation_overlay_layout(
        960, 640, hit_content, hit_layout));
    int64_t outside_point[] = {4, 4};
    const uint64_t outside_param = static_cast<uint64_t>(
        reinterpret_cast<uintptr_t>(outside_point));
    const int confirm_count_before_miss = g_confirm_callbacks;
    CHECK(host.dispatch_event(0x17, outside_param, 0) == 1);
    CHECK(host.dispatch_event(0x18, outside_param, 0) == 1);
    CHECK(g_confirm_callbacks == confirm_count_before_miss);
    int64_t confirm_point[] = {
        hit_layout.confirm_button.x + hit_layout.confirm_button.w / 2,
        hit_layout.confirm_button.y + hit_layout.confirm_button.h / 2};
    const uint64_t confirm_param = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(confirm_point));
    CHECK(host.dispatch_event(0x17, confirm_param, 0) == 1);
    CHECK(g_confirm_callbacks == confirm_count_before_miss);
    CHECK(host.dispatch_event(0x18, confirm_param, 0) == 1);
    CHECK(g_confirm_callbacks == confirm_count_before_miss + 1);
    CHECK(!qol::features::core_ui::ui_confirmation_modal_input_active());
    CHECK(!g_ui_state_dialog_active);

    // Failed unregister retains an inert owner record; re-enable re-registers the same owner.
    g_fail_overlay_unregister_once = true;
    CHECK(!qol::features::core_ui::ui_confirmation_set_installed(false));
    CHECK(qol::features::core_ui::ui_confirmation_set_installed(true));
    CHECK(qol::features::core_ui::ui_confirmation_register_builtin_frame_overlay());
    overlay_token = 0;
    CHECK(qol::contracts::succeeded(open_custom_confirmation(
        &overlay_token, 10, "重启后仍可绘制", nullptr, nullptr, nullptr)));
    const int component_draw_before_retry = g_component_draw_calls;
    host.dispatch_process();
    CHECK(g_component_draw_calls > component_draw_before_retry);
    CHECK(qol::features::core_ui::ui_confirmation_modal_input_active());
    const qol::plugins::custom_ui::UiConfirmationOverlayContent retry_content{
        "重启后仍可绘制", nullptr, nullptr, true, 10, nullptr};
    qol::plugins::custom_ui::UiConfirmationOverlayLayout retry_layout{};
    CHECK(qol::plugins::custom_ui::ui_confirmation_overlay_layout(
        960, 640, retry_content, retry_layout));
    int64_t cancel_point[] = {
        retry_layout.cancel_button.x + retry_layout.cancel_button.w / 2,
        retry_layout.cancel_button.y + retry_layout.cancel_button.h / 2};
    const uint64_t cancel_param = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(cancel_point));
    const int cancels_before_touch = g_cancel_callbacks;
    CHECK(host.dispatch_event(0x17, cancel_param, 0) == 1);
    CHECK(host.dispatch_event(0x18, cancel_param, 0) == 1);
    CHECK(g_cancel_callbacks == cancels_before_touch + 1);
    CHECK(!g_ui_state_dialog_active && !qol::features::core_ui::ui_confirmation_modal_input_active());
    overlay_token = 0;
    g_screen_inset_w = -1;
    g_screen_inset_h = -1;
    const int component_draw_before_bad_layout = g_component_draw_calls;
    CHECK(!qol::contracts::succeeded(open_custom_confirmation(
        &overlay_token, 10, "屏幕过小", nullptr, nullptr, nullptr)));
    CHECK(overlay_token == 0);
    CHECK(g_component_draw_calls == component_draw_before_bad_layout);
    CHECK(!qol::features::core_ui::ui_confirmation_modal_input_active());

    g_screen_inset_w = 0;
    g_screen_inset_h = 0;
    CHECK(qol::contracts::succeeded(open_custom_confirmation(
        &overlay_token, 10, "绘制失败", nullptr, nullptr, nullptr)));
    g_component_fail_fill = true;
    const int component_draw_before_failure = g_component_draw_calls;
    host.dispatch_process();
    CHECK(g_component_draw_calls == component_draw_before_failure + 1);
    CHECK(!qol::features::core_ui::ui_confirmation_modal_input_active());
    CHECK(!g_ui_state_dialog_active);
    g_component_fail_fill = false;
    qol::features::core_ui::ui_confirmation_set_installed(false);
    qol::features::core_ui::ui_confirmation_unbind();

    std::printf("core_frame_overlay_tests: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
