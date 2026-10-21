#include "contracts/ui_confirmation.h"
#include "plugins/core.custom_ui/ui_confirmation.h"

#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace {

using qol::contracts::QolUiAdapter;
using qol::contracts::QolUiPopupMsgCallbackFn;
using qol::contracts::UiConfirmationSpec;
using qol::contracts::UiConfirmationSpecText;

QolUiAdapter g_adapter{};
uint8_t g_popup_active = 0;
QolUiPopupMsgCallbackFn g_ok = nullptr;
QolUiPopupMsgCallbackFn g_cancel = nullptr;
QolUiPopupMsgCallbackFn g_original_ok = nullptr;
QolUiPopupMsgCallbackFn g_original_cancel = nullptr;
uint32_t g_text_id = 0;
uint32_t g_display_type = 0;
uint32_t g_format_arg = 0;
uint32_t g_native_param = 0;
int g_create_calls = 0;
int g_close_calls = 0;
bool g_fail_after_create = false;
bool g_fail_close = false;
bool g_remain_active_after_cancel = false;
int g_confirm_calls = 0;
int g_cancel_calls = 0;
int g_popup_draw_calls = 0;
void* g_last_user = nullptr;
uint64_t g_reentrant_token = 0;
QolResult g_reentrant_result{};
bool g_try_reentrant_open = false;
UiConfirmationSpec g_reentrant_spec{};
int g_raw_create_calls = 0;
const char* g_raw_pointer = nullptr;
uint32_t g_raw_length = 0;
uint32_t g_raw_display_type = 0;
uint32_t g_raw_param = 0;
char g_raw_copy[256]{};
const qol::contracts::UiConfirmationApi* g_api = nullptr;

void old_ok(void*) {}
void old_cancel(void*) {}
void foreign_ok(void*) {}
void foreign_cancel(void*) {}

int32_t is_main_thread() { return 1; }
QolResult popup_draw() { ++g_popup_draw_calls; return qol::contracts::ok(); }

QolResult popup_active(uint8_t* out) {
    if (out == nullptr) return qol::contracts::error(kQolErrInvalidArgument, "bad", 3);
    *out = g_popup_active;
    return qol::contracts::ok();
}

QolResult popup_get_callbacks(QolUiPopupMsgCallbackFn* out_ok,
                              QolUiPopupMsgCallbackFn* out_cancel) {
    if (out_ok == nullptr || out_cancel == nullptr) {
        return qol::contracts::error(kQolErrInvalidArgument, "bad", 3);
    }
    *out_ok = g_ok;
    *out_cancel = g_cancel;
    return qol::contracts::ok();
}

QolResult popup_replace_callback_if(uint8_t is_ok, QolUiPopupMsgCallbackFn expected,
                                    QolUiPopupMsgCallbackFn replacement, uint8_t* out) {
    if (is_ok > 1 || out == nullptr) {
        return qol::contracts::error(kQolErrInvalidArgument, "bad", 3);
    }
    *out = 0;
    QolUiPopupMsgCallbackFn& slot = is_ok != 0 ? g_ok : g_cancel;
    if (slot == expected) {
        slot = replacement;
        *out = 1;
    }
    return qol::contracts::ok();
}

QolResult popup_create(uint32_t text_id, uint32_t display_type, uint32_t format_arg,
                       QolUiPopupMsgCallbackFn ok, QolUiPopupMsgCallbackFn cancel,
                       uint32_t param) {
    ++g_create_calls;
    g_text_id = text_id;
    g_display_type = display_type;
    g_format_arg = format_arg;
    g_native_param = param;
    if (g_popup_active != 0) {
        return qol::contracts::error(kQolErrBusy, "busy", 4);
    }
    g_ok = ok;
    g_cancel = cancel;
    g_popup_active = 1;
    return g_fail_after_create
        ? qol::contracts::error(kQolErrBlocked, "failed", 6)
        : qol::contracts::ok();
}

QolResult popup_create_text(const char* text, uint32_t length, uint32_t display_type,
                            uint32_t param, QolUiPopupMsgCallbackFn ok,
                            QolUiPopupMsgCallbackFn cancel) {
    ++g_raw_create_calls;
    g_raw_pointer = text;
    g_raw_length = length;
    g_raw_display_type = display_type;
    g_raw_param = param;
    if (text != nullptr) {
        std::snprintf(g_raw_copy, sizeof(g_raw_copy), "%.*s", static_cast<int>(length), text);
    }
    if (g_popup_active != 0) {
        return qol::contracts::error(kQolErrBusy, "busy", 4);
    }
    g_ok = ok;
    g_cancel = cancel;
    g_popup_active = 1;
    return qol::contracts::ok();
}

QolResult popup_close_if_callbacks(QolUiPopupMsgCallbackFn expected_ok,
                                   QolUiPopupMsgCallbackFn expected_cancel,
                                   uint8_t* out_closed) {
    if (out_closed == nullptr) {
        return qol::contracts::error(kQolErrInvalidArgument, "bad", 3);
    }
    *out_closed = 0;
    if (g_popup_active == 0 || g_ok != expected_ok || g_cancel != expected_cancel) {
        return qol::contracts::error(kQolErrConflict, "not owner", 9);
    }
    ++g_close_calls;
    if (g_fail_close) {
        return qol::contracts::error(kQolErrNotReady, "native close failed", 19);
    }
    g_popup_active = 0;
    const auto callback = g_cancel;
    if (callback != nullptr) {
        callback(reinterpret_cast<void*>(static_cast<uintptr_t>(g_native_param)));
    }
    if (g_remain_active_after_cancel) {
        g_popup_active = 1;
        return qol::contracts::error(kQolErrConflict, "still active", 12);
    }
    *out_closed = 1;
    return qol::contracts::ok();
}

void on_confirm(void* user) {
    ++g_confirm_calls;
    g_last_user = user;
    if (g_try_reentrant_open) {
        g_try_reentrant_open = false;
        g_reentrant_result = g_api->open(&g_reentrant_spec, &g_reentrant_token);
    }
}
void on_cancel(void* user) {
    ++g_cancel_calls;
    g_last_user = user;
}

UiConfirmationSpec spec(uint32_t price, void* user) {
    return UiConfirmationSpec{321, 7, 0x87654321, price, on_confirm, on_cancel, user};
}

void reset_fake() {
    g_popup_active = 0;
    g_ok = g_original_ok = &old_ok;
    g_cancel = g_original_cancel = &old_cancel;
    g_text_id = g_display_type = g_format_arg = g_native_param = 0;
    g_create_calls = g_close_calls = 0;
    g_fail_after_create = false;
    g_fail_close = false;
    g_remain_active_after_cancel = false;
    g_confirm_calls = g_cancel_calls = 0;
    g_last_user = nullptr;
    g_reentrant_token = 0;
    g_reentrant_result = qol::contracts::ok();
    g_try_reentrant_open = false;
    g_raw_create_calls = 0;
    g_raw_pointer = nullptr;
    g_raw_length = g_raw_display_type = g_raw_param = 0;
    g_raw_copy[0] = '\0';
}

void trigger_confirm(bool b_on_cleared_first = false) {
    const auto callback = g_ok;
    assert(callback != nullptr);
    g_popup_active = b_on_cleared_first ? 0 : 1;
    callback(reinterpret_cast<void*>(static_cast<uintptr_t>(g_native_param)));
    g_popup_active = 0;
}

void trigger_cancel(bool b_on_cleared_first = false) {
    const auto callback = g_cancel;
    assert(callback != nullptr);
    g_popup_active = b_on_cleared_first ? 0 : 1;
    callback(reinterpret_cast<void*>(static_cast<uintptr_t>(g_native_param)));
    g_popup_active = 0;
}

}  // namespace

int main() {
    g_adapter.size = sizeof(g_adapter);
    g_adapter.major = qol::contracts::kUiAdapterMajor;
    g_adapter.minor = qol::contracts::kUiAdapterMinor;
    g_adapter.is_main_thread = &is_main_thread;
    g_adapter.popup_msg_active = &popup_active;
    g_adapter.popup_msg_get_callbacks = &popup_get_callbacks;
    g_adapter.popup_msg_replace_callback_if = &popup_replace_callback_if;
    g_adapter.popup_msg_create_yesno_from_textdata = &popup_create;
    g_adapter.create_yes_no_text = &popup_create_text;
    g_adapter.popup_msg_close_if_callbacks = &popup_close_if_callbacks;
    g_adapter.call_ui_popup_msg_draw = &popup_draw;
    assert(qol::features::core_ui::ui_confirmation_bind(&g_adapter));
    qol::features::core_ui::ui_confirmation_set_installed(true);
    g_api = qol::features::core_ui::ui_confirmation_api();
    assert(g_api != nullptr);

    // Numeric price is passed natively; the consumer context remains separate.
    reset_fake();
    g_popup_draw_calls = 0;
    int context = 0;
    auto request = spec(0x7fedcba9u, &context);
    uint64_t token = 0;
    assert(qol::contracts::succeeded(g_api->open(&request, &token)) && token != 0);
    assert(g_create_calls == 1 && g_text_id == 321 && g_display_type == 7);
    assert(g_format_arg == 0x87654321 && g_native_param == 0x7fedcba9u);
    assert(g_ok != nullptr && g_cancel != nullptr && g_ok != &old_ok && g_cancel != &old_cancel);
    uint8_t active = 0;
    assert(qol::contracts::succeeded(g_api->active(token, &active)) && active == 1);
    assert(g_api->active(token + 1, &active).code == kQolErrNotFound && active == 0);
    assert(g_api->close(token + 1).code == kQolErrNotFound);
    assert(g_popup_active == 1 && g_close_calls == 0);
    assert(g_popup_draw_calls == 0);  // Native-page V1 must not composite from the panel seam.

    const auto stale_ok = g_ok;
    trigger_confirm();
    assert(g_confirm_calls == 1 && g_cancel_calls == 0 && g_last_user == &context);
    assert(g_ok == g_original_ok && g_cancel == g_original_cancel);
    stale_ok(reinterpret_cast<void*>(static_cast<uintptr_t>(g_native_param)));
    assert(g_confirm_calls == 1);
    assert(g_api->active(token, &active).code == kQolErrNotFound);
    assert(g_api->close(token).code == kQolErrNotFound);

    // v1.1 raw-text open: provider copies the message, uses the raw adapter slot, and keeps
    // the copy alive through the callback stack.
    reset_fake();
    char local_message[256] = "任意字符串：确认执行？";
    UiConfirmationSpecText text_request{};
    text_request.size = sizeof(text_request);
    std::snprintf(text_request.message, sizeof(text_request.message), "%s", local_message);
    text_request.display_type = 4;
    text_request.price = 77;
    text_request.on_confirm = &on_confirm;
    text_request.on_cancel = &on_cancel;
    text_request.user = &context;
    uint64_t text_token = 0;
    assert(qol::contracts::succeeded(g_api->open_text(&text_request, &text_token)) &&
           text_token != 0);
    assert(g_raw_create_calls == 1 && g_create_calls == 0);
    assert(g_raw_pointer != local_message);
    assert(g_raw_length == std::strlen(local_message) &&
           std::strcmp(g_raw_pointer, local_message) == 0);
    assert(g_raw_display_type == 4 && g_raw_param == 77);
    std::memset(local_message, 0, sizeof(local_message));
    assert(std::strcmp(g_raw_copy, "任意字符串：确认执行？") == 0);
    trigger_confirm();
    assert(g_confirm_calls == 1);
    assert(g_raw_pointer[0] == '\0');  // released after consumer stack exits
    assert(g_api->active(text_token, &active).code == kQolErrNotFound);

    // Invalid raw specs are rejected before touching the native popup.
    reset_fake();
    UiConfirmationSpecText invalid = text_request;
    invalid.size = sizeof(invalid) - 1;
    token = 0;
    assert(g_api->open_text(&invalid, &token).code == kQolErrInvalidArgument && token == 0);
    invalid = text_request;
    std::memset(invalid.message, 'x', sizeof(invalid.message));
    assert(g_api->open_text(&invalid, &token).code == kQolErrInvalidArgument);
    invalid = text_request;
    invalid.price = 0x80000000u;
    assert(g_api->open_text(&invalid, &token).code == kQolErrInvalidArgument);
    invalid = text_request;
    invalid.message[0] = '\0';
    assert(g_api->open_text(&invalid, &token).code == kQolErrInvalidArgument);
    assert(g_raw_create_calls == 0);

    // A reentrant open from the consumer callback is rejected until dispatch completes.
    reset_fake();
    request = spec(11, &context);
    assert(qol::contracts::succeeded(g_api->open(&request, &token)));
    g_reentrant_spec = spec(12, nullptr);
    g_try_reentrant_open = true;
    trigger_confirm();
    assert(g_confirm_calls == 1 && g_reentrant_result.code == kQolErrBusy);
    assert(g_reentrant_token == 0 && g_create_calls == 1);
    request = spec(13, &context);
    uint64_t next_token = 0;
    assert(qol::contracts::succeeded(g_api->open(&request, &next_token)));
    assert(next_token != token);
    assert(g_api->close(token).code == kQolErrNotFound);
    assert(g_popup_active == 1 && g_close_calls == 0 &&
           qol::contracts::succeeded(g_api->active(next_token, &active)) && active == 1);
    trigger_cancel();
    assert(g_cancel_calls == 1 && g_last_user == &context);

    // Preserve the native ordering variant where ButtonExe clears bOn before fp callback.
    reset_fake();
    request = spec(14, &context);
    assert(qol::contracts::succeeded(g_api->open(&request, &token)));
    trigger_confirm(true);
    assert(g_confirm_calls == 1 && g_cancel_calls == 0 && g_last_user == &context);
    assert(g_ok == g_original_ok && g_cancel == g_original_cancel);

    reset_fake();
    request = spec(15, &context);
    assert(qol::contracts::succeeded(g_api->open(&request, &token)));
    trigger_cancel(true);
    assert(g_confirm_calls == 0 && g_cancel_calls == 1 && g_last_user == &context);
    assert(g_ok == g_original_ok && g_cancel == g_original_cancel);

    // Busy rejects an already-active foreign UIPopupMsg without changing its callbacks.
    reset_fake();
    g_popup_active = 1;
    g_ok = &foreign_ok;
    g_cancel = &foreign_cancel;
    request = spec(20, &context);
    token = 999;
    const auto busy = g_api->open(&request, &token);
    assert(busy.code == kQolErrBusy && token == 0 && g_create_calls == 0);
    assert(g_ok == &foreign_ok && g_cancel == &foreign_cancel && g_popup_active == 1);

    // High-bit prices are rejected before touching the popup or owner state.
    reset_fake();
    request = spec(0x80000000u, &context);
    token = 99;
    assert(g_api->open(&request, &token).code == kQolErrInvalidArgument && token == 0);
    assert(g_create_calls == 0 && g_popup_active == 0 && g_ok == g_original_ok &&
           g_cancel == g_original_cancel);

    // Duplicate open leaves the existing owner and token intact; cancel is consumed once.
    reset_fake();
    request = spec(45, &context);
    assert(qol::contracts::succeeded(g_api->open(&request, &token)));
    auto duplicate = spec(99, nullptr);
    uint64_t duplicate_token = 123;
    assert(g_api->open(&duplicate, &duplicate_token).code == kQolErrBusy);
    assert(duplicate_token == 0 && g_create_calls == 1 && g_native_param == 45);
    const auto stale_cancel = g_cancel;
    trigger_cancel();
    assert(g_confirm_calls == 0 && g_cancel_calls == 1 && g_last_user == &context);
    assert(g_ok == g_original_ok && g_cancel == g_original_cancel);
    stale_cancel(reinterpret_cast<void*>(static_cast<uintptr_t>(g_native_param)));
    assert(g_cancel_calls == 1);

    // Active mixed-owner callbacks do not write either slot or dispatch; inactive cleanup
    // restores only our slot and leaves the foreign slot intact.
    reset_fake();
    request = spec(67, &context);
    assert(qol::contracts::succeeded(g_api->open(&request, &token)));
    const auto owned_ok = g_ok;
    const auto owned_cancel = g_cancel;
    g_ok = &foreign_ok;
    g_popup_active = 1;
    owned_cancel(reinterpret_cast<void*>(static_cast<uintptr_t>(g_native_param)));
    assert(g_confirm_calls == 0 && g_cancel_calls == 0);
    assert(g_ok == &foreign_ok && g_cancel == owned_cancel);
    assert(qol::contracts::succeeded(g_api->active(token, &active)) && active == 0);
    assert(g_ok == &foreign_ok && g_cancel == owned_cancel);
    g_popup_active = 0;
    assert(qol::contracts::succeeded(g_api->active(token, &active)) && active == 0);
    assert(g_ok == &foreign_ok && g_cancel == g_original_cancel);
    assert(g_api->close(token).code == kQolErrNotFound && g_close_calls == 0);

    // A stale owned trampoline is rejected as a prior callback, not saved for the next owner.
    g_cancel = owned_cancel;
    uint64_t rejected_token = 99;
    assert(g_api->open(&request, &rejected_token).code == kQolErrConflict);
    assert(rejected_token == 0 && g_create_calls == 1 && g_cancel == owned_cancel);
    g_cancel = g_original_cancel;

    // Reopen/close after inactive mixed cleanup preserves foreign callbacks and no stale trampoline.
    uint64_t reopened_token = 0;
    assert(qol::contracts::succeeded(g_api->open(&request, &reopened_token)));
    assert(reopened_token != token && g_popup_active == 1);
    assert(qol::contracts::succeeded(g_api->close(reopened_token)));
    assert(g_ok == &foreign_ok && g_cancel == g_original_cancel && g_close_calls == 1);

    // open() itself also cleans a prior closing owner after bOn becomes inactive.
    reset_fake();
    request = spec(69, &context);
    assert(qol::contracts::succeeded(g_api->open(&request, &token)));
    const auto pending_cancel = g_cancel;
    g_ok = &foreign_ok;
    pending_cancel(reinterpret_cast<void*>(static_cast<uintptr_t>(g_native_param)));
    assert(g_popup_active == 1 && g_cancel == pending_cancel && g_ok == &foreign_ok);
    g_popup_active = 0;
    request = spec(70, &context);
    assert(qol::contracts::succeeded(g_api->open(&request, &reopened_token)));
    assert(g_ok != &foreign_ok && g_cancel != g_original_cancel);
    assert(qol::contracts::succeeded(g_api->close(reopened_token)));
    assert(g_ok == &foreign_ok && g_cancel == g_original_cancel);

    // A close request with an externally replaced slot neither closes nor writes either slot.
    reset_fake();
    request = spec(68, &context);
    assert(qol::contracts::succeeded(g_api->open(&request, &token)));
    const auto close_owned_ok = g_ok;
    const auto close_owned_cancel = g_cancel;
    g_ok = &foreign_ok;
    assert(g_api->close(token).code == kQolErrConflict);
    assert(g_popup_active == 1 && g_close_calls == 0 && g_ok == &foreign_ok &&
           g_cancel == close_owned_cancel);
    g_ok = close_owned_ok;
    assert(qol::contracts::succeeded(g_api->close(token)));
    assert(g_popup_active == 0 && g_close_calls == 1 && g_ok == g_original_ok &&
           g_cancel == g_original_cancel && g_cancel_calls == 0);

    // Native close failure retains owner state and both callbacks for a later retry.
    reset_fake();
    request = spec(88, &context);
    assert(qol::contracts::succeeded(g_api->open(&request, &token)));
    const auto retry_ok = g_ok;
    const auto retry_cancel = g_cancel;
    g_fail_close = true;
    assert(g_api->close(token).code == kQolErrNotReady);
    assert(g_popup_active == 1 && g_ok == retry_ok && g_cancel == retry_cancel);
    assert(qol::contracts::succeeded(g_api->active(token, &active)) && active == 1);
    assert(g_confirm_calls == 0 && g_cancel_calls == 0);
    g_fail_close = false;
    assert(qol::contracts::succeeded(g_api->close(token)));
    assert(g_popup_active == 0 && g_ok == g_original_ok && g_cancel == g_original_cancel);
    assert(g_confirm_calls == 0 && g_cancel_calls == 0);

    // A close that returns but leaves bOn set is also retryable and cannot clear callbacks.
    reset_fake();
    request = spec(89, &context);
    assert(qol::contracts::succeeded(g_api->open(&request, &token)));
    const auto still_active_ok = g_ok;
    const auto still_active_cancel = g_cancel;
    g_remain_active_after_cancel = true;
    assert(g_api->close(token).code == kQolErrConflict);
    assert(g_popup_active == 1 && g_ok == still_active_ok && g_cancel == still_active_cancel);
    assert(qol::contracts::succeeded(g_api->active(token, &active)) && active == 1);
    g_remain_active_after_cancel = false;
    assert(qol::contracts::succeeded(g_api->close(token)));
    assert(g_popup_active == 0 && g_ok == g_original_ok && g_cancel == g_original_cancel);

    // Failed open cleanup closes only after native inactivity; failed cleanup remains owned.
    reset_fake();
    g_fail_after_create = true;
    g_fail_close = true;
    request = spec(123, &context);
    token = 0;
    assert(g_api->open(&request, &token).code == kQolErrBlocked && token != 0);
    assert(g_popup_active == 1 && g_close_calls == 1 && g_confirm_calls == 0 && g_cancel_calls == 0);
    assert(g_ok != g_original_ok && g_cancel != g_original_cancel);
    g_fail_close = false;
    assert(qol::contracts::succeeded(g_api->close(token)));
    assert(g_popup_active == 0 && g_ok == g_original_ok && g_cancel == g_original_cancel);

    reset_fake();
    g_fail_after_create = true;
    request = spec(122, &context);
    token = 999;
    assert(g_api->open(&request, &token).code == kQolErrBlocked && token == 0);
    assert(g_popup_active == 0 && g_close_calls == 1 &&
           g_ok == g_original_ok && g_cancel == g_original_cancel);

    // Disable/deactivate suppresses business callbacks and preserves retry state on close failure.
    reset_fake();
    request = spec(124, &context);
    assert(qol::contracts::succeeded(g_api->open(&request, &token)));
    g_fail_close = true;
    qol::features::core_ui::ui_confirmation_set_installed(false);
    assert(g_popup_active == 1 && g_ok != g_original_ok && g_cancel != g_original_cancel);
    const auto disabled_cancel = g_cancel;
    trigger_cancel();  // stale/native callback while disabled must not dispatch or clear ownership.
    assert(g_confirm_calls == 0 && g_cancel_calls == 0 && g_cancel == disabled_cancel);
    g_fail_close = false;
    assert(qol::contracts::succeeded(qol::features::core_ui::ui_confirmation_close_owned()));
    assert(g_popup_active == 0 && g_ok == g_original_ok && g_cancel == g_original_cancel);

    qol::features::core_ui::ui_confirmation_unbind();
    std::puts("core_ui_confirmation_tests: pass");
    return 0;
}
