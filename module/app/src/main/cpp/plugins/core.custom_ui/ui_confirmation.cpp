#include "ui_confirmation.h"

#include <cstddef>
#include <cstdint>
#include <limits>
#include <mutex>

namespace qol::features::core_ui {
namespace {

using Callback = contracts::UiConfirmationCallback;
using NativeCallback = contracts::QolUiPopupMsgCallbackFn;

std::mutex g_mutex;
const contracts::QolUiAdapter* g_adapter = nullptr;
bool g_installed = false;
bool g_active = false;
bool g_opening = false;
bool g_closing = false;
bool g_consuming = false;
bool g_dispatching = false;
uint64_t g_generation = 0;
uint64_t g_next_token = 1;
uint64_t g_active_token = 0;
contracts::UiConfirmationSpec g_spec{};
NativeCallback g_previous_ok = nullptr;
NativeCallback g_previous_cancel = nullptr;

constexpr char kInvalidArgument[] = "invalid confirmation spec or token";
constexpr char kNotReady[] = "confirmation adapter unavailable";
constexpr char kBusy[] = "UIPopupMsg already active";
constexpr char kBlocked[] = "ui_confirmation disabled";
constexpr char kOwnershipChanged[] = "UIPopupMsg ownership changed";
constexpr char kStaleToken[] = "ui_confirmation token is stale";
constexpr char kPriceOutOfRange[] = "confirmation price exceeds INT32_MAX";

void confirmation_ok_trampoline(void*);
void confirmation_cancel_trampoline(void*);

QolResult error(int32_t code, const char* message, uint32_t length) {
    return contracts::error(code, message, length);
}

QolResult not_ready() {
    return error(kQolErrNotReady, kNotReady, sizeof(kNotReady) - 1);
}

QolResult stale_token() {
    return error(kQolErrNotFound, kStaleToken, sizeof(kStaleToken) - 1);
}

bool adapter_has_confirmation_slots(const contracts::QolUiAdapter* adapter) {
    if (adapter == nullptr || adapter->major != contracts::kUiAdapterMajor ||
        adapter->minor < contracts::kUiAdapterMinor) {
        return false;
    }
    constexpr size_t required_size =
        offsetof(contracts::QolUiAdapter, popup_msg_close_if_callbacks) +
        sizeof(((contracts::QolUiAdapter*)nullptr)->popup_msg_close_if_callbacks);
    return adapter->size >= required_size && adapter->is_main_thread != nullptr &&
        adapter->popup_msg_active != nullptr && adapter->popup_msg_get_callbacks != nullptr &&
        adapter->popup_msg_replace_callback_if != nullptr &&
        adapter->popup_msg_create_yesno_from_textdata != nullptr &&
        adapter->popup_msg_close_if_callbacks != nullptr;
}

const contracts::QolUiAdapter* adapter_snapshot() {
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_adapter;
}

bool on_main_thread(const contracts::QolUiAdapter* adapter) {
    return adapter_has_confirmation_slots(adapter) && adapter->is_main_thread() != 0;
}

uint64_t clear_locked() {
    const uint64_t previous_generation = g_generation;
    g_active = false;
    g_opening = false;
    g_closing = false;
    g_consuming = false;
    g_active_token = 0;
    g_spec = {};
    g_previous_ok = nullptr;
    g_previous_cancel = nullptr;
    ++g_generation;
    return previous_generation;
}

bool read_popup(const contracts::QolUiAdapter* adapter, uint8_t* out_active,
                NativeCallback* out_ok, NativeCallback* out_cancel) {
    if (adapter->popup_msg_active(out_active).ok != kQolOk) return false;
    if (out_ok != nullptr || out_cancel != nullptr) {
        if (out_ok == nullptr || out_cancel == nullptr ||
            adapter->popup_msg_get_callbacks(out_ok, out_cancel).ok != kQolOk) {
            return false;
        }
    }
    return true;
}

// Active callbacks require complete-pair ownership; confirmed-inactive cleanup may restore only
// slots that still match this provider and must leave every foreign slot untouched.
bool restore_callbacks(const contracts::QolUiAdapter* adapter,
                       NativeCallback previous_ok, NativeCallback previous_cancel,
                       bool allow_partial, bool* out_both_owned) {
    if (out_both_owned != nullptr) *out_both_owned = false;
    NativeCallback current_ok = nullptr;
    NativeCallback current_cancel = nullptr;
    if (adapter->popup_msg_get_callbacks(&current_ok, &current_cancel).ok != kQolOk) return false;
    if (current_ok != &confirmation_ok_trampoline ||
        current_cancel != &confirmation_cancel_trampoline) {
        if (!allow_partial) return true;
    }
    const bool both_owned = current_ok == &confirmation_ok_trampoline &&
        current_cancel == &confirmation_cancel_trampoline;
    if (out_both_owned != nullptr) *out_both_owned = both_owned;

    if (current_ok == &confirmation_ok_trampoline) {
        uint8_t replaced_ok = 0;
        if (adapter->popup_msg_replace_callback_if(1, &confirmation_ok_trampoline,
                                                   previous_ok, &replaced_ok).ok != kQolOk ||
            replaced_ok == 0) {
            return false;
        }
    }
    if (current_cancel == &confirmation_cancel_trampoline) {
        uint8_t replaced_cancel = 0;
        if (adapter->popup_msg_replace_callback_if(0, &confirmation_cancel_trampoline,
                                                   previous_cancel, &replaced_cancel).ok != kQolOk ||
            replaced_cancel == 0) {
            return false;
        }
    }
    return true;
}

bool restore_own_callbacks(const contracts::QolUiAdapter* adapter,
                           NativeCallback previous_ok, NativeCallback previous_cancel,
                           bool* out_both_owned) {
    return restore_callbacks(adapter, previous_ok, previous_cancel, false, out_both_owned);
}

bool clear_if_inactive(const contracts::QolUiAdapter* adapter, uint64_t token,
                       NativeCallback previous_ok, NativeCallback previous_cancel) {
    uint8_t popup_active = 1;
    NativeCallback current_ok = nullptr;
    NativeCallback current_cancel = nullptr;
    if (!read_popup(adapter, &popup_active, &current_ok, &current_cancel) || popup_active != 0) {
        return false;
    }
    bool both_owned = false;
    if (!restore_callbacks(adapter, previous_ok, previous_cancel, true, &both_owned)) return false;
    if (!both_owned) {
        // In an inactive mixed-owner popup, retain every foreign slot and prove no owned
        // trampoline remains before dropping the retry state.
        NativeCallback after_ok = nullptr;
        NativeCallback after_cancel = nullptr;
        if (adapter->popup_msg_get_callbacks(&after_ok, &after_cancel).ok != kQolOk ||
            after_ok == &confirmation_ok_trampoline ||
            after_cancel == &confirmation_cancel_trampoline) {
            return false;
        }
    }
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_active && g_active_token == token) {
        clear_locked();
        return true;
    }
    return false;
}

QolResult confirmation_close_token(uint64_t token, bool require_token_match) {
    const contracts::QolUiAdapter* adapter = adapter_snapshot();
    if (!on_main_thread(adapter)) return not_ready();

    NativeCallback previous_ok = nullptr;
    NativeCallback previous_cancel = nullptr;
    uint64_t owned_token = 0;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (!g_active) return require_token_match ? stale_token() : contracts::ok();
        if (require_token_match && (token == 0 || token != g_active_token)) return stale_token();
        owned_token = g_active_token;
        previous_ok = g_previous_ok;
        previous_cancel = g_previous_cancel;
        g_closing = true;
    }

    uint8_t popup_active = 1;
    NativeCallback current_ok = nullptr;
    NativeCallback current_cancel = nullptr;
    if (!read_popup(adapter, &popup_active, &current_ok, &current_cancel)) return not_ready();
    if (popup_active == 0) {
        if (!clear_if_inactive(adapter, owned_token, previous_ok, previous_cancel)) return not_ready();
        std::lock_guard<std::mutex> lock(g_mutex);
        return g_active && g_active_token == owned_token ? not_ready() : contracts::ok();
    }

    // A mixed/foreign pair is not ours to close or repair. Keep the owner state for retry.
    if (current_ok != &confirmation_ok_trampoline ||
        current_cancel != &confirmation_cancel_trampoline) {
        return error(kQolErrConflict, kOwnershipChanged, sizeof(kOwnershipChanged) - 1);
    }

    uint8_t closed = 0;
    const QolResult native_close = adapter->popup_msg_close_if_callbacks(
        &confirmation_ok_trampoline, &confirmation_cancel_trampoline, &closed);

    // ButtonCancelExe invokes the cancel trampoline synchronously. g_closing makes that
    // trampoline a no-op; only this outer frame may clean the slots/state after bOn is reread.
    popup_active = 1;
    current_ok = nullptr;
    current_cancel = nullptr;
    if (!read_popup(adapter, &popup_active, &current_ok, &current_cancel)) {
        return native_close.ok == kQolOk
            ? not_ready()
            : native_close;
    }
    if (popup_active != 0) {
        if (native_close.ok != kQolOk) return native_close;
        return error(kQolErrConflict, kOwnershipChanged, sizeof(kOwnershipChanged) - 1);
    }

    if (!clear_if_inactive(adapter, owned_token, previous_ok, previous_cancel)) return not_ready();
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (g_active && g_active_token == owned_token) return not_ready();
    }
    (void)closed;
    return contracts::ok();
}

void confirmation_dispatch(bool confirm) {
    const contracts::QolUiAdapter* adapter = adapter_snapshot();
    if (!on_main_thread(adapter)) return;

    contracts::UiConfirmationSpec spec{};
    NativeCallback previous_ok = nullptr;
    NativeCallback previous_cancel = nullptr;
    uint64_t token = 0;
    uint64_t generation = 0;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (!g_active || g_closing || g_consuming || !g_installed) return;
        g_consuming = true;
        spec = g_spec;
        previous_ok = g_previous_ok;
        previous_cancel = g_previous_cancel;
        token = g_active_token;
        generation = g_generation;
    }

    bool both_owned = false;
    if (!restore_own_callbacks(adapter, previous_ok, previous_cancel, &both_owned)) {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (g_active && g_generation == generation) {
            g_consuming = false;
            g_closing = true;
        }
        return;
    }
    if (!both_owned) {
        uint8_t popup_active = 1;
        if (adapter->popup_msg_active(&popup_active).ok != kQolOk) {
            std::lock_guard<std::mutex> lock(g_mutex);
            if (g_active && g_generation == generation && g_active_token == token) {
                g_consuming = false;
                g_closing = true;
            }
            return;
        }
        if (popup_active == 0 &&
            clear_if_inactive(adapter, token, previous_ok, previous_cancel)) {
            return;
        }
        std::lock_guard<std::mutex> lock(g_mutex);
        if (g_active && g_generation == generation && g_active_token == token) {
            g_consuming = false;
            g_closing = true;
        }
        return;
    }

    Callback consumer = nullptr;
    void* user = nullptr;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (!g_active || g_generation != generation || g_active_token != token) return;
        if (both_owned) {
            consumer = confirm ? spec.on_confirm : spec.on_cancel;
            user = spec.user;
        }
        clear_locked();
        if (consumer != nullptr) g_dispatching = true;
    }

    if (consumer != nullptr) {
        consumer(user);
        std::lock_guard<std::mutex> lock(g_mutex);
        g_dispatching = false;
    }
}

void confirmation_ok_trampoline(void*) { confirmation_dispatch(true); }
void confirmation_cancel_trampoline(void*) { confirmation_dispatch(false); }

void clear_after_failed_open(const contracts::QolUiAdapter* adapter, uint64_t token) {
    NativeCallback previous_ok = nullptr;
    NativeCallback previous_cancel = nullptr;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (!g_active || g_active_token != token) return;
        g_closing = true;
        previous_ok = g_previous_ok;
        previous_cancel = g_previous_cancel;
    }
    clear_if_inactive(adapter, token, previous_ok, previous_cancel);
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (!g_active || g_active_token != token) return;
    }
    (void)confirmation_close_token(token, false);
}

void expose_retry_token_if_retained(uint64_t token, uint64_t* out_token) {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_active && g_active_token == token) *out_token = token;
}

QolResult confirmation_open(const contracts::UiConfirmationSpec* spec, uint64_t* out_token) {
    if (out_token == nullptr) {
        return error(kQolErrInvalidArgument, kInvalidArgument, sizeof(kInvalidArgument) - 1);
    }
    *out_token = 0;
    if (spec == nullptr || spec->on_confirm == nullptr || spec->on_cancel == nullptr) {
        return error(kQolErrInvalidArgument, kInvalidArgument, sizeof(kInvalidArgument) - 1);
    }
    if (spec->price > static_cast<uint32_t>(std::numeric_limits<int32_t>::max())) {
        return error(kQolErrInvalidArgument, kPriceOutOfRange, sizeof(kPriceOutOfRange) - 1);
    }
    const contracts::QolUiAdapter* adapter = adapter_snapshot();
    if (!on_main_thread(adapter)) return not_ready();

    uint64_t closing_token = 0;
    NativeCallback closing_previous_ok = nullptr;
    NativeCallback closing_previous_cancel = nullptr;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (!g_installed) return error(kQolErrBlocked, kBlocked, sizeof(kBlocked) - 1);
        if (g_dispatching || g_opening) {
            return error(kQolErrBusy, kBusy, sizeof(kBusy) - 1);
        }
        if (g_active) {
            if (!g_closing) return error(kQolErrBusy, kBusy, sizeof(kBusy) - 1);
            closing_token = g_active_token;
            closing_previous_ok = g_previous_ok;
            closing_previous_cancel = g_previous_cancel;
        }
    }
    if (closing_token != 0) {
        uint8_t closing_popup_active = 1;
        if (adapter->popup_msg_active(&closing_popup_active).ok != kQolOk) return not_ready();
        if (closing_popup_active != 0) {
            return error(kQolErrBusy, kBusy, sizeof(kBusy) - 1);
        }
        if (!clear_if_inactive(adapter, closing_token, closing_previous_ok,
                               closing_previous_cancel)) {
            return not_ready();
        }
    }

    uint8_t popup_active = 0;
    if (adapter->popup_msg_active(&popup_active).ok != kQolOk) return not_ready();
    if (popup_active != 0) return error(kQolErrBusy, kBusy, sizeof(kBusy) - 1);

    NativeCallback previous_ok = nullptr;
    NativeCallback previous_cancel = nullptr;
    if (adapter->popup_msg_get_callbacks(&previous_ok, &previous_cancel).ok != kQolOk) {
        return not_ready();
    }
    if (previous_ok == &confirmation_ok_trampoline ||
        previous_cancel == &confirmation_cancel_trampoline) {
        return error(kQolErrConflict, kOwnershipChanged, sizeof(kOwnershipChanged) - 1);
    }

    uint64_t token = 0;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (!g_installed) return error(kQolErrBlocked, kBlocked, sizeof(kBlocked) - 1);
        if (g_dispatching || g_active || g_opening) {
            return error(kQolErrBusy, kBusy, sizeof(kBusy) - 1);
        }
        if (g_next_token == 0) {
            static constexpr char message[] = "confirmation token space exhausted";
            return error(kQolErrBlocked, message, sizeof(message) - 1);
        }
        token = g_next_token;
        g_next_token = token == UINT64_MAX ? 0 : token + 1;
        g_active = true;
        g_opening = true;
        g_closing = false;
        g_consuming = false;
        g_active_token = token;
        g_spec = *spec;
        g_previous_ok = previous_ok;
        g_previous_cancel = previous_cancel;
        ++g_generation;
    }

    const QolResult created = adapter->popup_msg_create_yesno_from_textdata(
        spec->text_id, spec->display_type, spec->format_arg,
        &confirmation_ok_trampoline, &confirmation_cancel_trampoline, spec->price);
    if (created.ok != kQolOk) {
        clear_after_failed_open(adapter, token);
        expose_retry_token_if_retained(token, out_token);
        return created;
    }

    NativeCallback current_ok = nullptr;
    NativeCallback current_cancel = nullptr;
    popup_active = 0;
    const bool opened = read_popup(adapter, &popup_active, &current_ok, &current_cancel) &&
        popup_active != 0 && current_ok == &confirmation_ok_trampoline &&
        current_cancel == &confirmation_cancel_trampoline;
    if (!opened) {
        clear_after_failed_open(adapter, token);
        expose_retry_token_if_retained(token, out_token);
        return error(kQolErrConflict, kOwnershipChanged, sizeof(kOwnershipChanged) - 1);
    }

    {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (!g_active || g_active_token != token) {
            return error(kQolErrConflict, kOwnershipChanged, sizeof(kOwnershipChanged) - 1);
        }
        g_opening = false;
    }
    *out_token = token;
    return contracts::ok();
}

QolResult confirmation_active(uint64_t token, uint8_t* out_active) {
    if (out_active == nullptr || token == 0) {
        return error(kQolErrInvalidArgument, kInvalidArgument, sizeof(kInvalidArgument) - 1);
    }
    *out_active = 0;
    const contracts::QolUiAdapter* adapter = adapter_snapshot();
    if (!on_main_thread(adapter)) return not_ready();
    NativeCallback previous_ok = nullptr;
    NativeCallback previous_cancel = nullptr;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (!g_active || g_active_token != token) return stale_token();
        previous_ok = g_previous_ok;
        previous_cancel = g_previous_cancel;
    }
    uint8_t popup_active = 0;
    NativeCallback current_ok = nullptr;
    NativeCallback current_cancel = nullptr;
    if (!read_popup(adapter, &popup_active, &current_ok, &current_cancel)) return not_ready();
    if (popup_active == 0) {
        return clear_if_inactive(adapter, token, previous_ok, previous_cancel)
            ? contracts::ok() : not_ready();
    }
    if (current_ok == &confirmation_ok_trampoline &&
        current_cancel == &confirmation_cancel_trampoline) {
        *out_active = 1;
    } else {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (g_active && g_active_token == token) g_closing = true;
    }
    return contracts::ok();
}

QolResult confirmation_close(uint64_t token) {
    if (token == 0) return error(kQolErrInvalidArgument, kInvalidArgument,
                                 sizeof(kInvalidArgument) - 1);
    return confirmation_close_token(token, true);
}

constexpr contracts::UiConfirmationApi kApi{
    &confirmation_open,
    &confirmation_active,
    &confirmation_close,
};

}  // namespace

bool ui_confirmation_bind(const contracts::QolUiAdapter* adapter) {
    if (!adapter_has_confirmation_slots(adapter)) return false;
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_active || g_opening || g_dispatching) return g_adapter == adapter;
    g_adapter = adapter;
    return true;
}

void ui_confirmation_unbind() {
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_installed = false;
        if (g_active) g_closing = true;
    }
    (void)ui_confirmation_close_owned();
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!g_active) g_adapter = nullptr;
}

void ui_confirmation_set_installed(bool installed) {
    if (!installed) {
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            g_installed = false;
            if (g_active) g_closing = true;
        }
        (void)ui_confirmation_close_owned();
        return;
    }

    bool retry_close = false;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        retry_close = g_active && g_closing;
        if (!retry_close) g_installed = true;
    }
    if (retry_close) {
        (void)ui_confirmation_close_owned();
        std::lock_guard<std::mutex> lock(g_mutex);
        if (!g_active) g_installed = true;
    }
}

QolResult ui_confirmation_close_owned() {
    uint64_t token = 0;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (!g_active) return contracts::ok();
        token = g_active_token;
    }
    return confirmation_close_token(token, false);
}

const contracts::UiConfirmationApi* ui_confirmation_api() { return &kApi; }

}  // namespace qol::features::core_ui
