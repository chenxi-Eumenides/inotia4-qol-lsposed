#include "ui_confirmation.h"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <mutex>

#include "panels/host.h"

namespace qol::features::core_ui {
namespace {

using Callback = contracts::UiConfirmationCallback;
using NativeCallback = contracts::QolUiPopupMsgCallbackFn;

std::mutex g_mutex;
const contracts::QolUiAdapter* g_adapter = nullptr;
const contracts::QolGameHost* g_game_host = nullptr;
const contracts::QolHostSdk* g_sdk = nullptr;
bool g_installed = false;
bool g_active = false;
bool g_opening = false;
bool g_closing = false;
bool g_consuming = false;
bool g_dispatching = false;
bool g_overlay_registered = false;
bool g_overlay_orphaned = false;
bool g_overlay_drawn_once = false;
bool g_v1_overlay = false;
bool g_return_suppression_pending = false;
int64_t g_return_suppression_frame = -1;
uint64_t g_generation = 0;
uint64_t g_next_token = 1;
uint64_t g_active_token = 0;
contracts::UiConfirmationSpec g_spec{};
// Raw-text dialogs keep this buffer alive until bOn==0 and the consumer callback exits.
char g_text_buffer[sizeof(contracts::UiConfirmationSpecText().message)]{};
bool g_raw_text = false;
NativeCallback g_previous_ok = nullptr;
NativeCallback g_previous_cancel = nullptr;
contracts::QolUiFrameOverlayDrawFn g_overlay_draw = nullptr;
void* g_overlay_user = nullptr;

struct OverlayBindingTag {};
OverlayBindingTag g_overlay_binding_tag;

struct OverlayScopeOwner {
    uint64_t id = 0;
    uint64_t token = 0;
    bool adapter_ended = false;
};
constexpr uint32_t kOverlayScopeOwnerCapacity = 64;
OverlayScopeOwner g_overlay_scope_owners[kOverlayScopeOwnerCapacity]{};
uint32_t g_overlay_scope_owner_count = 0;
uint64_t g_next_overlay_scope_owner = 1;

constexpr char kInvalidArgument[] = "invalid confirmation spec or token";
constexpr char kNotReady[] = "confirmation adapter unavailable";
constexpr char kBusy[] = "UIPopupMsg already active";
constexpr char kBlocked[] = "ui_confirmation disabled";
constexpr char kOwnershipChanged[] = "UIPopupMsg ownership changed";
constexpr char kStaleToken[] = "ui_confirmation token is stale";
constexpr char kPriceOutOfRange[] = "confirmation price exceeds INT32_MAX";
constexpr char kTextTooLarge[] = "confirmation text is not NUL-terminated within 256 bytes";
constexpr char kTextUnsupported[] = "confirmation adapter lacks raw-text creation";

void confirmation_ok_trampoline(void*);
void confirmation_cancel_trampoline(void*);
uint64_t overlay_scope_owner_begin(uint64_t token);
bool overlay_scope_owner_finish(uint64_t id);
void overlay_scope_owner_release(uint64_t id);
bool overlay_scope_owner_finish_current(uint64_t token);

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
        adapter->minor < 27) {
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

bool adapter_has_text_slots(const contracts::QolUiAdapter* adapter) {
    if (adapter == nullptr || adapter->major != contracts::kUiAdapterMajor ||
        adapter->minor < 32 || adapter->size < offsetof(contracts::QolUiAdapter,
                                                          create_yes_no_text) +
                                      sizeof(adapter->create_yes_no_text)) {
        return false;
    }
    return adapter->create_yes_no_text != nullptr;
}

bool adapter_has_overlay_slots(const contracts::QolUiAdapter* adapter) {
    if (adapter == nullptr || adapter->major != contracts::kUiAdapterMajor ||
        adapter->minor < 28) {
        return false;
    }
    constexpr size_t required_size =
        offsetof(contracts::QolUiAdapter, set_frame_overlay) +
        sizeof(((contracts::QolUiAdapter*)nullptr)->set_frame_overlay);
    return adapter->size >= required_size && adapter->set_frame_overlay != nullptr;
}

bool adapter_has_end_overlay_scope(const contracts::QolUiAdapter* adapter) {
    if (adapter == nullptr || adapter->major != contracts::kUiAdapterMajor ||
        adapter->minor < 28 ||
        adapter->size < offsetof(contracts::QolUiAdapter, end_overlay_scope) +
                            sizeof(adapter->end_overlay_scope)) {
        return false;
    }
    return adapter->end_overlay_scope != nullptr;
}

int64_t current_game_frame() {
    return g_game_host != nullptr && g_game_host->thread.frame_count != nullptr
        ? g_game_host->thread.frame_count() : -1;
}

uint64_t overlay_scope_owner_begin(uint64_t token) {
    if (token == 0) return 0;
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_overlay_scope_owner_count >= kOverlayScopeOwnerCapacity ||
        g_next_overlay_scope_owner == 0) return 0;
    const uint64_t id = g_next_overlay_scope_owner;
    g_next_overlay_scope_owner = id == UINT64_MAX ? 0 : id + 1;
    g_overlay_scope_owners[g_overlay_scope_owner_count++] = {id, token, false};
    return id;
}

bool overlay_scope_owner_finish(uint64_t id) {
    if (id == 0) return false;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        for (uint32_t i = 0; i < g_overlay_scope_owner_count; ++i) {
            if (g_overlay_scope_owners[i].id == id &&
                g_overlay_scope_owners[i].adapter_ended) return true;
        }
    }
    const contracts::QolUiAdapter* adapter = nullptr;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        adapter = g_adapter;
    }
    if (!adapter_has_end_overlay_scope(adapter)) return false;
    if (!contracts::succeeded(adapter->end_overlay_scope())) return false;
    std::lock_guard<std::mutex> lock(g_mutex);
    for (uint32_t i = 0; i < g_overlay_scope_owner_count; ++i) {
        if (g_overlay_scope_owners[i].id == id) {
            g_overlay_scope_owners[i].adapter_ended = true;
            return true;
        }
    }
    return false;
}

void overlay_scope_owner_release(uint64_t id) {
    if (id == 0) return;
    std::lock_guard<std::mutex> lock(g_mutex);
    for (uint32_t i = 0; i < g_overlay_scope_owner_count; ++i) {
        if (g_overlay_scope_owners[i].id != id) continue;
        for (uint32_t j = i; j + 1 < g_overlay_scope_owner_count; ++j) {
            g_overlay_scope_owners[j] = g_overlay_scope_owners[j + 1];
        }
        g_overlay_scope_owners[--g_overlay_scope_owner_count] = {};
        return;
    }
}

bool overlay_scope_owner_finish_current(uint64_t token) {
    uint64_t id = 0;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        for (uint32_t i = g_overlay_scope_owner_count; i > 0; --i) {
            if (g_overlay_scope_owners[i - 1].token == token) {
                id = g_overlay_scope_owners[i - 1].id;
                break;
            }
        }
    }
    return id != 0 && overlay_scope_owner_finish(id);
}

bool confirmation_overlay_draw(void*, contracts::QolUiAdapter* ui, uint64_t token) {
    if (ui == nullptr || token == 0) return false;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (!g_active || !g_installed || g_closing || g_active_token != token ||
            !g_v1_overlay) {
            return false;
        }
    }
    uint8_t native_active = 0;
    if (ui->popup_msg_active == nullptr ||
        !contracts::succeeded(ui->popup_msg_active(&native_active))) {
        return false;
    }
    if (native_active == 0) {
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            if (g_active && g_active_token == token) {
                g_overlay_drawn_once = false;
                g_v1_overlay = false;
            }
        }
        (void)overlay_scope_owner_finish_current(token);
        return false;
    }
    if (ui->call_ui_popup_msg_draw == nullptr ||
        !contracts::succeeded(ui->call_ui_popup_msg_draw())) {
        if (g_sdk != nullptr && g_sdk->log.write != nullptr) {
            static constexpr char message[] = "native popup draw failed; V1 remains invisible";
            (void)g_sdk->log.write(1, 12, "core.custom_ui/ui_confirmation", message,
                                   sizeof(message) - 1);
        }
        return false;
    }
    return true;
}

bool confirmation_frame_overlay_dispatch(void* user, contracts::QolUiAdapter* ui,
                                         uint64_t token) {
    if (user != &g_overlay_binding_tag || ui == nullptr || token == 0) return false;
    contracts::QolUiFrameOverlayDrawFn draw = nullptr;
    void* draw_user = nullptr;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (!g_installed || !g_active || g_closing || !g_overlay_registered ||
            g_overlay_orphaned || !g_v1_overlay || g_active_token != token) {
            return false;
        }
        draw = g_overlay_draw;
        draw_user = g_overlay_user;
    }
    if (draw == nullptr || !draw(draw_user, ui, token)) return false;
    ui_confirmation_overlay_draw_succeeded(token);
    return true;
}

bool unregister_frame_overlay(const contracts::QolUiAdapter* adapter) {
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (!g_overlay_registered) return true;
        g_overlay_orphaned = true;
    }
    if (!adapter_has_overlay_slots(adapter)) return false;
    const QolResult result = adapter->set_frame_overlay(nullptr, &g_overlay_binding_tag);
    if (!contracts::succeeded(result)) return false;
    std::lock_guard<std::mutex> lock(g_mutex);
    g_overlay_registered = false;
    g_overlay_orphaned = false;
    g_overlay_draw = nullptr;
    g_overlay_user = nullptr;
    return true;
}

const contracts::QolUiAdapter* adapter_snapshot() {
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_adapter;
}

bool on_main_thread(const contracts::QolUiAdapter* adapter) {
    return adapter_has_confirmation_slots(adapter) && adapter->is_main_thread() != 0;
}

uint64_t clear_locked(bool clear_message = true) {
    const uint64_t previous_generation = g_generation;
    g_active = false;
    g_opening = false;
    g_closing = false;
    g_consuming = false;
    g_overlay_drawn_once = false;
    g_v1_overlay = false;
    g_active_token = 0;
    g_spec = {};
    if (clear_message) {
        std::memset(g_text_buffer, 0, sizeof(g_text_buffer));
        g_raw_text = false;
    }
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

    if (current_ok != &confirmation_ok_trampoline ||
        current_cancel != &confirmation_cancel_trampoline) {
        return error(kQolErrConflict, kOwnershipChanged, sizeof(kOwnershipChanged) - 1);
    }

    uint8_t closed = 0;
    const QolResult native_close = adapter->popup_msg_close_if_callbacks(
        &confirmation_ok_trampoline, &confirmation_cancel_trampoline, &closed);

    popup_active = 1;
    current_ok = nullptr;
    current_cancel = nullptr;
    if (!read_popup(adapter, &popup_active, &current_ok, &current_cancel)) {
        return native_close.ok == kQolOk ? not_ready() : native_close;
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
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (!g_active || g_closing || g_consuming || !g_installed) return;
    }
    const contracts::QolUiAdapter* adapter = adapter_snapshot();
    if (!on_main_thread(adapter)) return;
    const int64_t callback_frame = confirm ? -1 : current_game_frame();

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
        if (!confirm) {
            if (callback_frame >= 0) {
                g_return_suppression_pending = true;
                g_return_suppression_frame = callback_frame;
            }
        }
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
        consumer = confirm ? spec.on_confirm : spec.on_cancel;
        user = spec.user;
        // Keep the raw-text buffer valid until bOn==0 and the consumer stack exits.
        clear_locked(false);
        if (consumer != nullptr) g_dispatching = true;
    }

    if (consumer != nullptr) {
        consumer(user);
        std::lock_guard<std::mutex> lock(g_mutex);
        g_dispatching = false;
    }
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (!g_active) {
            std::memset(g_text_buffer, 0, sizeof(g_text_buffer));
            g_raw_text = false;
        }
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

QolResult confirmation_open_impl(const contracts::UiConfirmationSpec* spec,
                                 const char* raw_text, uint32_t raw_length, bool raw_source,
                                 uint64_t* out_token) {
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
    if (raw_source && !adapter_has_text_slots(adapter)) {
        return error(kQolErrUnsupported, kTextUnsupported, sizeof(kTextUnsupported) - 1);
    }

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
        g_overlay_drawn_once = false;
        g_v1_overlay = false;
        g_return_suppression_pending = false;
        g_return_suppression_frame = -1;
        g_spec = *spec;
        std::memset(g_text_buffer, 0, sizeof(g_text_buffer));
        if (raw_source && raw_text != nullptr) {
            // Include the NUL so the native side can also treat it as a C string.
            std::memcpy(g_text_buffer, raw_text, raw_length + 1);
        }
        g_raw_text = raw_source;
        g_previous_ok = previous_ok;
        g_previous_cancel = previous_cancel;
        ++g_generation;
    }

    const QolResult created = raw_source
        ? adapter->create_yes_no_text(g_text_buffer, raw_length, 0,
                                      spec->price, &confirmation_ok_trampoline,
                                      &confirmation_cancel_trampoline)
        : adapter->popup_msg_create_yesno_from_textdata(
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

    // Experimental V1 compositing: when a custom panel is the live top provider, probe the
    // frame scope so the panel frame can call the native UIPopupMsg_Draw. Probe failure is
    // non-fatal: V1 semantics stay as before (popup invisible on custom panels).
    bool v1_overlay = false;
    if (adapter->begin_overlay_scope != nullptr && adapter->call_ui_popup_msg_draw != nullptr) {
        const void* provider = nullptr;
        uint64_t generation = 0;
        if (ui_panels_custom_overlay_scope_identity(&provider, &generation) &&
            contracts::succeeded(adapter->begin_overlay_scope(token, provider, generation))) {
            const uint64_t owner = overlay_scope_owner_begin(token);
            if (owner != 0) {
                (void)overlay_scope_owner_finish(owner);
                overlay_scope_owner_release(owner);
                v1_overlay = true;
            } else if (adapter_has_end_overlay_scope(adapter)) {
                (void)adapter->end_overlay_scope();
            }
        }
    }

    {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (!g_active || g_active_token != token) {
            return error(kQolErrConflict, kOwnershipChanged, sizeof(kOwnershipChanged) - 1);
        }
        g_v1_overlay = v1_overlay;
        g_opening = false;
    }
    *out_token = token;
    return contracts::ok();
}

QolResult confirmation_open(const contracts::UiConfirmationSpec* spec, uint64_t* out_token) {
    return confirmation_open_impl(spec, nullptr, 0, false, out_token);
}

QolResult confirmation_open_text(const contracts::UiConfirmationSpecText* spec,
                                 uint64_t* out_token) {
    if (out_token == nullptr) {
        return error(kQolErrInvalidArgument, kInvalidArgument, sizeof(kInvalidArgument) - 1);
    }
    *out_token = 0;
    if (spec == nullptr || spec->size < sizeof(contracts::UiConfirmationSpecText) ||
        spec->on_confirm == nullptr || spec->on_cancel == nullptr) {
        return error(kQolErrInvalidArgument, kInvalidArgument, sizeof(kInvalidArgument) - 1);
    }
    if (spec->price > static_cast<uint32_t>(std::numeric_limits<int32_t>::max())) {
        return error(kQolErrInvalidArgument, kPriceOutOfRange, sizeof(kPriceOutOfRange) - 1);
    }
    const void* terminator = std::memchr(spec->message, '\0', sizeof(spec->message));
    if (terminator == nullptr) {
        return error(kQolErrInvalidArgument, kTextTooLarge, sizeof(kTextTooLarge) - 1);
    }
    const uint32_t length = static_cast<uint32_t>(
        static_cast<const char*>(terminator) - spec->message);
    if (length == 0) {
        return error(kQolErrInvalidArgument, kInvalidArgument, sizeof(kInvalidArgument) - 1);
    }
    contracts::UiConfirmationSpec normalized{};
    normalized.price = spec->price;
    normalized.on_confirm = spec->on_confirm;
    normalized.on_cancel = spec->on_cancel;
    normalized.user = spec->user;
    return confirmation_open_impl(&normalized, spec->message, length, true, out_token);
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
    &confirmation_open_text,
};

}  // namespace

bool ui_confirmation_bind(const contracts::QolUiAdapter* adapter,
                          const contracts::QolGameHost* game,
                          const contracts::QolHostSdk* sdk) {
    if (!adapter_has_confirmation_slots(adapter)) return false;
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_active || g_opening || g_dispatching) return g_adapter == adapter;
    g_adapter = adapter;
    g_game_host = game;
    g_sdk = sdk;
    return true;
}

void ui_confirmation_unbind() {
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_installed = false;
        g_return_suppression_pending = false;
        g_return_suppression_frame = -1;
        if (g_active) g_closing = true;
    }
    (void)unregister_frame_overlay(adapter_snapshot());
    (void)ui_confirmation_close_owned();
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!g_active) {
        g_adapter = nullptr;
        g_game_host = nullptr;
        g_sdk = nullptr;
    }
}

bool ui_confirmation_set_installed(bool installed) {
    if (!installed) {
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            g_installed = false;
            g_return_suppression_pending = false;
            g_return_suppression_frame = -1;
            if (g_active) g_closing = true;
        }
        const bool unregistered = unregister_frame_overlay(adapter_snapshot());
        const QolResult closed = ui_confirmation_close_owned();
        return unregistered && contracts::succeeded(closed);
    }

    bool retry_close = false;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        retry_close = g_active && g_closing;
        if (!retry_close) g_installed = true;
    }
    if (retry_close) {
        const QolResult closed = ui_confirmation_close_owned();
        std::lock_guard<std::mutex> lock(g_mutex);
        if (!g_active && contracts::succeeded(closed)) g_installed = true;
        return !g_active && contracts::succeeded(closed);
    }
    return true;
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

uint64_t ui_confirmation_overlay_scope_begin(uint64_t token) {
    return overlay_scope_owner_begin(token);
}

bool ui_confirmation_overlay_scope_finish(uint64_t scope_id) {
    return overlay_scope_owner_finish(scope_id);
}

void ui_confirmation_overlay_scope_release(uint64_t scope_id) {
    overlay_scope_owner_release(scope_id);
}

bool ui_confirmation_overlay_scope_finish_current(uint64_t token) {
    return overlay_scope_owner_finish_current(token);
}

bool ui_confirmation_register_frame_overlay(contracts::QolUiFrameOverlayDrawFn draw,
                                            void* user) {
    if (draw == nullptr) return false;
    const contracts::QolUiAdapter* adapter = adapter_snapshot();
    if (!adapter_has_overlay_slots(adapter) || adapter->is_main_thread() == 0) return false;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (g_opening || g_closing) return false;
        if (g_overlay_registered) {
            if (g_overlay_draw != draw || g_overlay_user != user) return false;
            if (!g_overlay_orphaned) return true;
        } else {
            g_overlay_draw = draw;
            g_overlay_user = user;
            g_overlay_registered = true;
            g_overlay_orphaned = true;
        }
    }
    const QolResult result = adapter->set_frame_overlay(&confirmation_frame_overlay_dispatch,
                                                        &g_overlay_binding_tag);
    if (contracts::succeeded(result)) {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_overlay_orphaned = false;
        return true;
    }
    std::lock_guard<std::mutex> lock(g_mutex);
    g_overlay_orphaned = true;
    return false;
}

bool ui_confirmation_register_builtin_frame_overlay() {
    const contracts::QolUiAdapter* adapter = adapter_snapshot();
    if (!adapter_has_overlay_slots(adapter)) {
        std::lock_guard<std::mutex> lock(g_mutex);
        return !g_overlay_registered;
    }
    return ui_confirmation_register_frame_overlay(&confirmation_overlay_draw, nullptr);
}

bool ui_confirmation_modal_overlay_token(uint64_t* out_token) {
    if (out_token == nullptr) return false;
    *out_token = 0;
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!g_active || !g_installed || g_closing || !g_v1_overlay || g_active_token == 0) {
        return false;
    }
    *out_token = g_active_token;
    return true;
}

bool ui_confirmation_modal_input_active() {
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_active && g_v1_overlay && g_overlay_drawn_once && g_active_token != 0;
}

bool ui_confirmation_consume_return_suppression() {
    const int64_t frame = current_game_frame();
    std::lock_guard<std::mutex> lock(g_mutex);
    const bool pending = g_return_suppression_pending && frame >= 0 &&
        frame == g_return_suppression_frame;
    g_return_suppression_pending = false;
    g_return_suppression_frame = -1;
    return pending;
}

void ui_confirmation_overlay_draw_succeeded(uint64_t token) {
    if (token == 0) return;
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_active && g_v1_overlay && g_active_token == token) {
        g_overlay_drawn_once = true;
    }
}

uint32_t ui_confirmation_dispatch_custom_event(uint64_t event, uint64_t param,
                                               uint64_t param2) {
    (void)event;
    (void)param;
    (void)param2;
    // V1 composited popups never route touch into module hit-testing: native button hit
    // testing stays in the game control tree. Only the panel-side event is consumed.
    return 1;
}

void ui_confirmation_overlay_scope_finished(uint64_t token) {
    (void)token;
    // V1 semantics are never cancelled by a missing first draw.
}

bool ui_confirmation_cancel_for_return() {
    bool v1_modal = false;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        v1_modal = g_active && g_active_token != 0 && g_v1_overlay && g_overlay_drawn_once;
    }
    // V1 composited popup: the game's UIPopupMsg_Process owns Return and clears bOn itself.
    // Consume the panel-side F3 so the parent provider cannot close on the same key.
    return v1_modal;
}

}  // namespace qol::features::core_ui
