#include "ui_confirmation.h"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <mutex>

#include "feature.h"
#include "panels/host.h"
#include "ui_confirmation_overlay.h"

namespace qol::features::core_ui {
namespace {

using Callback = contracts::UiConfirmationCallback;
using NativeCallback = contracts::QolUiPopupMsgCallbackFn;

enum class Backend : uint8_t { kNativeTextData, kNativeRaw, kCustomOverlay };

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
bool g_return_suppression_pending = false;
int64_t g_return_suppression_frame = -1;
uint64_t g_generation = 0;
uint64_t g_next_token = 1;
uint64_t g_active_token = 0;
contracts::UiConfirmationSpec g_spec{};
Backend g_backend = Backend::kNativeTextData;
uint64_t g_custom_dialog_generation = 0;
char g_native_message[256]{};
NativeCallback g_previous_ok = nullptr;
NativeCallback g_previous_cancel = nullptr;
contracts::QolUiFrameOverlayDrawFn g_overlay_draw = nullptr;
void* g_overlay_user = nullptr;

struct OverlayText {
    char message[257]{};
    char confirm_label[65]{};
    char cancel_label[65]{};
    char price_unit[65]{};
    bool valid = false;
};

OverlayText g_overlay_text{};
qol::plugins::custom_ui::UiConfirmationOverlayLayout g_overlay_layout{};
uint64_t g_overlay_layout_token = 0;
qol::plugins::custom_ui::UiConfirmationOverlayHit g_pressed_hit =
    qol::plugins::custom_ui::UiConfirmationOverlayHit::kNone;
uint64_t g_pressed_token = 0;

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

bool adapter_has_raw_yesno_slot(const contracts::QolUiAdapter* adapter) {
    if (adapter == nullptr || adapter->major != contracts::kUiAdapterMajor ||
        adapter->minor < 29) {
        return false;
    }
    constexpr size_t required_size =
        offsetof(contracts::QolUiAdapter, popup_msg_create_yesno) +
        sizeof(((contracts::QolUiAdapter*)nullptr)->popup_msg_create_yesno);
    return adapter->size >= required_size && adapter->popup_msg_create_yesno != nullptr;
}

const contracts::UiStateApi* ui_state_api() {
    if (g_sdk == nullptr || g_sdk->capabilities.require == nullptr) return nullptr;
    const void* iface = nullptr;
    const QolResult result = g_sdk->capabilities.require(
        contracts::kUiStateCapabilityId,
        {contracts::kUiStateVersionMajor, contracts::kUiStateVersionMinor}, &iface);
    return contracts::succeeded(result) && iface != nullptr
        ? static_cast<const contracts::UiStateApi*>(iface) : nullptr;
}

int64_t current_game_frame() {
    return g_game_host != nullptr && g_game_host->thread.frame_count != nullptr
        ? g_game_host->thread.frame_count() : -1;
}

bool overlay_content_preflight(const contracts::QolUiAdapter* adapter,
                               const qol::plugins::custom_ui::UiConfirmationOverlayContent& content,
                               qol::plugins::custom_ui::UiConfirmationOverlayLayout* out_layout) {
    if (adapter == nullptr || out_layout == nullptr || adapter->screen_inset_width == nullptr ||
        adapter->screen_inset_height == nullptr || adapter->fill_rect_alpha == nullptr ||
        adapter->draw_string_with_font == nullptr) {
        return false;
    }
    int32_t inset_w = 0;
    int32_t inset_h = 0;
    if (!contracts::succeeded(adapter->screen_inset_width(&inset_w)) ||
        !contracts::succeeded(adapter->screen_inset_height(&inset_h)) ||
        inset_w < 0 || inset_h < 0) {
        return false;
    }
    const uint64_t screen_w = 960u + static_cast<uint64_t>(inset_w) * 2u;
    const uint64_t screen_h = 640u + static_cast<uint64_t>(inset_h) * 2u;
    if (screen_w > UINT32_MAX || screen_h > UINT32_MAX) return false;
    return qol::plugins::custom_ui::ui_confirmation_overlay_layout(
        static_cast<uint32_t>(screen_w), static_cast<uint32_t>(screen_h), content, *out_layout);
}

QolResult publish_custom_dialog_v2(const contracts::UiConfirmationSpec& spec,
                                   const OverlayText& text, uint64_t sequence,
                                   uint64_t generation) {
    const contracts::UiStateApi* api = ui_state_api();
    if (api == nullptr || api->publish_custom_dialog_v2 == nullptr) return not_ready();
    contracts::UiCustomDialogSnapshotV2 snapshot{};
    snapshot.size = sizeof(snapshot);
    snapshot.sequence = sequence;
    snapshot.generation = generation;
    std::snprintf(snapshot.type_id, sizeof(snapshot.type_id), "%s", "ui_confirmation");
    std::snprintf(snapshot.body, sizeof(snapshot.body), "%s", text.message);
    std::snprintf(snapshot.confirm_label, sizeof(snapshot.confirm_label), "%s",
                  text.confirm_label);
    std::snprintf(snapshot.cancel_label, sizeof(snapshot.cancel_label), "%s",
                  text.cancel_label);
    snapshot.has_price = spec.price != 0 ? 1 : 0;
    snapshot.price = static_cast<int32_t>(spec.price);
    std::snprintf(snapshot.price_unit, sizeof(snapshot.price_unit), "%s", text.price_unit);
    return api->publish_custom_dialog_v2(&snapshot);
}

QolResult clear_custom_dialog_v2(uint64_t generation) {
    const contracts::UiStateApi* api = ui_state_api();
    if (api == nullptr || api->clear_custom_dialog == nullptr) return not_ready();
    const uint64_t sequence = ui_dialog_sequence_next();
    if (sequence == 0) {
        static constexpr char message[] = "custom dialog sequence exhausted";
        return contracts::error(kQolErrBlocked, message, sizeof(message) - 1);
    }
    return api->clear_custom_dialog(generation, sequence);
}

bool confirmation_overlay_draw(void*, contracts::QolUiAdapter* ui, uint64_t token) {
    if (ui == nullptr || token == 0) return false;
    contracts::UiConfirmationSpec spec{};
    OverlayText text{};
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (!g_active || g_backend != Backend::kCustomOverlay || !g_installed || g_closing ||
            g_active_token != token ||
            !g_overlay_text.valid) {
            return false;
        }
        spec = g_spec;
        text = g_overlay_text;
    }

    const bool has_price = spec.price != 0;
    const qol::plugins::custom_ui::UiConfirmationOverlayContent content{
        text.message,
        text.confirm_label[0] == '\0' ? nullptr : text.confirm_label,
        text.cancel_label[0] == '\0' ? nullptr : text.cancel_label,
        has_price,
        spec.price,
        text.price_unit[0] == '\0' ? nullptr : text.price_unit,
    };
    qol::plugins::custom_ui::UiConfirmationOverlayLayout layout{};
    if (!overlay_content_preflight(ui, content, &layout) ||
        !qol::plugins::custom_ui::ui_confirmation_overlay_draw(*ui, content, layout)) {
        return false;
    }
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (!g_active || g_active_token != token) return false;
        g_overlay_layout = layout;
        g_overlay_layout_token = token;
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
            g_overlay_orphaned || g_backend != Backend::kCustomOverlay ||
            g_active_token != token) {
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
    if (!adapter_has_overlay_slots(adapter)) {
        return false;
    }
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

bool adapter_has_end_overlay_scope(const contracts::QolUiAdapter* adapter) {
    if (adapter == nullptr || adapter->major != contracts::kUiAdapterMajor ||
        adapter->minor < 28 ||
        adapter->size < offsetof(contracts::QolUiAdapter, end_overlay_scope) +
                            sizeof(adapter->end_overlay_scope)) {
        return false;
    }
    return adapter->end_overlay_scope != nullptr;
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
    const contracts::QolUiAdapter* adapter = adapter_snapshot();
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

bool on_main_thread(const contracts::QolUiAdapter* adapter) {
    return adapter_has_confirmation_slots(adapter) && adapter->is_main_thread() != 0;
}

uint64_t clear_locked() {
    const uint64_t previous_generation = g_generation;
    g_active = false;
    g_opening = false;
    g_closing = false;
    g_consuming = false;
    g_overlay_drawn_once = false;
    g_overlay_text = {};
    g_overlay_layout = {};
    g_overlay_layout_token = 0;
    g_pressed_hit = qol::plugins::custom_ui::UiConfirmationOverlayHit::kNone;
    g_pressed_token = 0;
    g_active_token = 0;
    g_spec = {};
    g_backend = Backend::kNativeTextData;
    g_custom_dialog_generation = 0;
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

void confirmation_dispatch_custom(bool confirm) {
    const contracts::QolUiAdapter* adapter = adapter_snapshot();
    if (!on_main_thread(adapter)) return;
    Callback consumer = nullptr;
    void* user = nullptr;
    uint64_t token = 0;
    uint64_t dialog_generation = 0;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (!g_active || g_backend != Backend::kCustomOverlay || g_closing || g_consuming ||
            !g_installed) {
            return;
        }
        g_consuming = true;
        consumer = confirm ? g_spec.on_confirm : g_spec.on_cancel;
        user = g_spec.user;
        token = g_active_token;
        dialog_generation = g_custom_dialog_generation;
    }
    (void)overlay_scope_owner_finish_current(token);
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (!g_active || g_backend != Backend::kCustomOverlay || g_active_token != token) return;
        clear_locked();
        if (consumer != nullptr) g_dispatching = true;
    }
    if (dialog_generation != 0) (void)clear_custom_dialog_v2(dialog_generation);
    if (consumer != nullptr) {
        consumer(user);
        std::lock_guard<std::mutex> lock(g_mutex);
        g_dispatching = false;
    }
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
    uint64_t custom_generation = 0;
    bool custom_backend = false;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (!g_active) return require_token_match ? stale_token() : contracts::ok();
        if (require_token_match && (token == 0 || token != g_active_token)) return stale_token();
        owned_token = g_active_token;
        custom_backend = g_backend == Backend::kCustomOverlay;
        if (custom_backend) custom_generation = g_custom_dialog_generation;
        previous_ok = g_previous_ok;
        previous_cancel = g_previous_cancel;
        g_closing = true;
    }

    if (custom_backend) {
        (void)overlay_scope_owner_finish_current(owned_token);
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            if (!g_active || g_active_token != owned_token) {
                return require_token_match ? stale_token() : contracts::ok();
            }
            clear_locked();
        }
        return custom_generation == 0 ? contracts::ok()
                                      : clear_custom_dialog_v2(custom_generation);
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
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (g_active && g_backend == Backend::kCustomOverlay) {
            // Custom backend does not install native UIPopupMsg callbacks.
            return;
        }
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
        if (!confirm && g_backend != Backend::kCustomOverlay) {
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

QolResult confirmation_open_native(const contracts::UiConfirmationSpec* spec, uint64_t* out_token,
                                   bool raw_message, const char* message,
                                   uint32_t message_length) {
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
    if (raw_message && (!adapter_has_raw_yesno_slot(adapter) || message == nullptr ||
                        message_length == 0 || message_length >= sizeof(g_native_message))) {
        return not_ready();
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
        g_return_suppression_pending = false;
        g_return_suppression_frame = -1;
        g_overlay_text = {};
        g_overlay_layout = {};
        g_overlay_layout_token = 0;
        g_pressed_hit = qol::plugins::custom_ui::UiConfirmationOverlayHit::kNone;
        g_pressed_token = 0;
        g_backend = raw_message ? Backend::kNativeRaw : Backend::kNativeTextData;
        g_custom_dialog_generation = 0;
        std::memset(g_native_message, 0, sizeof(g_native_message));
        if (raw_message) std::memcpy(g_native_message, message, message_length);
        g_spec = *spec;
        g_previous_ok = previous_ok;
        g_previous_cancel = previous_cancel;
        ++g_generation;
    }

    const QolResult created = raw_message
        ? adapter->popup_msg_create_yesno(
              g_native_message, message_length, 0, 2,
              &confirmation_ok_trampoline, &confirmation_cancel_trampoline, spec->price)
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

QolResult confirmation_open(const contracts::UiConfirmationSpec* spec, uint64_t* out_token) {
    return confirmation_open_native(spec, out_token, false, nullptr, 0);
}

bool fixed_string_length(const char* value, size_t capacity, size_t* out_length) {
    if (value == nullptr || out_length == nullptr) return false;
    const void* terminator = std::memchr(value, '\0', capacity);
    if (terminator == nullptr) return false;
    *out_length = static_cast<const char*>(terminator) - value;
    return true;
}

QolResult confirmation_open_custom_v2(const contracts::UiConfirmationSpec& base,
                                      const OverlayText& text, uint64_t* out_token) {
    const contracts::QolUiAdapter* adapter = adapter_snapshot();
    if (!on_main_thread(adapter)) return not_ready();
    if (adapter->popup_msg_active == nullptr) return not_ready();
    uint8_t native_active = 0;
    if (adapter->popup_msg_active(&native_active).ok != kQolOk) return not_ready();
    if (native_active != 0) {
        return error(kQolErrBusy, kBusy, sizeof(kBusy) - 1);
    }
    const contracts::UiStateApi* state = ui_state_api();
    if (state == nullptr || state->publish_custom_dialog_v2 == nullptr ||
        state->clear_custom_dialog == nullptr) {
        return not_ready();
    }
    // The overlay is drawn from the top provider's process scope. A provider without a
    // process callback would never open that scope, so custom mode is unavailable there.
    const void* scope_provider = nullptr;
    uint64_t scope_generation = 0;
    if (!ui_panels_custom_overlay_scope_identity(&scope_provider, &scope_generation)) {
        static constexpr char message[] = "custom overlay render scope unavailable";
        return error(kQolErrNotReady, message, sizeof(message) - 1);
    }
    const qol::plugins::custom_ui::UiConfirmationOverlayContent content{
        text.message,
        text.confirm_label[0] == '\0' ? nullptr : text.confirm_label,
        text.cancel_label[0] == '\0' ? nullptr : text.cancel_label,
        base.price != 0,
        base.price,
        text.price_unit[0] == '\0' ? nullptr : text.price_unit,
    };
    qol::plugins::custom_ui::UiConfirmationOverlayLayout preflight{};
    if (!overlay_content_preflight(adapter, content, &preflight)) return not_ready();

    uint64_t token = 0;
    uint64_t sequence = 0;
    uint64_t dialog_generation = 0;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (!g_installed) return error(kQolErrBlocked, kBlocked, sizeof(kBlocked) - 1);
        if (g_dispatching || g_active || g_opening) {
            return error(kQolErrBusy, kBusy, sizeof(kBusy) - 1);
        }
        if (g_next_token == 0) {
            static constexpr char exhausted[] = "confirmation token space exhausted";
            return error(kQolErrBlocked, exhausted, sizeof(exhausted) - 1);
        }
        if (!g_overlay_registered || g_overlay_orphaned) return not_ready();
        sequence = ui_dialog_sequence_next();
        dialog_generation = ui_dialog_generation_next();
        if (sequence == 0 || dialog_generation == 0) {
            static constexpr char exhausted[] = "custom dialog sequence space exhausted";
            return error(kQolErrBlocked, exhausted, sizeof(exhausted) - 1);
        }
        token = g_next_token;
        g_next_token = token == UINT64_MAX ? 0 : token + 1;
        g_active = true;
        g_opening = true;
        g_closing = false;
        g_consuming = false;
        g_active_token = token;
        g_backend = Backend::kCustomOverlay;
        g_overlay_drawn_once = false;
        g_return_suppression_pending = false;
        g_return_suppression_frame = -1;
        g_overlay_text = text;
        g_overlay_layout = preflight;
        g_overlay_layout_token = 0;
        g_pressed_hit = qol::plugins::custom_ui::UiConfirmationOverlayHit::kNone;
        g_pressed_token = 0;
        g_spec = base;
        std::memset(g_native_message, 0, sizeof(g_native_message));
        g_previous_ok = nullptr;
        g_previous_cancel = nullptr;
        g_custom_dialog_generation = dialog_generation;
        ++g_generation;
    }

    const QolResult published = publish_custom_dialog_v2(base, text, sequence, dialog_generation);
    if (!contracts::succeeded(published)) {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (g_active && g_active_token == token) clear_locked();
        return published;
    }
    const QolResult scope_opened = adapter->begin_overlay_scope != nullptr
        ? adapter->begin_overlay_scope(token, scope_provider, scope_generation)
        : contracts::error(kQolErrNotReady, "overlay scope slot unavailable",
                           sizeof("overlay scope slot unavailable") - 1);
    const uint64_t scope_owner = contracts::succeeded(scope_opened)
        ? ui_confirmation_overlay_scope_begin(token) : 0;
    if (!contracts::succeeded(scope_opened) || scope_owner == 0) {
        if (scope_owner != 0) {
            (void)ui_confirmation_overlay_scope_finish(scope_owner);
            ui_confirmation_overlay_scope_release(scope_owner);
        } else if (adapter_has_end_overlay_scope(adapter)) {
            (void)adapter->end_overlay_scope();
        }
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            if (g_active && g_active_token == token) clear_locked();
        }
        (void)clear_custom_dialog_v2(dialog_generation);
        return contracts::succeeded(scope_opened)
            ? error(kQolErrInternal, "overlay scope owner unavailable",
                    sizeof("overlay scope owner unavailable") - 1)
            : scope_opened;
    }
    // The open-time probe only validates the seam; the next dispatch_process arms the frame.
    (void)ui_confirmation_overlay_scope_finish(scope_owner);
    ui_confirmation_overlay_scope_release(scope_owner);
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (!g_active || g_active_token != token) {
            return error(kQolErrConflict, kStaleToken, sizeof(kStaleToken) - 1);
        }
        g_opening = false;
    }
    *out_token = token;
    return contracts::ok();
}

QolResult confirmation_open_v2(const contracts::UiConfirmationSpecV2* spec,
                               uint64_t* out_token) {
    if (out_token == nullptr) {
        return error(kQolErrInvalidArgument, kInvalidArgument, sizeof(kInvalidArgument) - 1);
    }
    *out_token = 0;
    if (spec == nullptr || spec->size < sizeof(contracts::UiConfirmationSpecV2) ||
        spec->on_confirm == nullptr || spec->on_cancel == nullptr ||
        spec->price > static_cast<uint32_t>(std::numeric_limits<int32_t>::max())) {
        return error(kQolErrInvalidArgument, kInvalidArgument, sizeof(kInvalidArgument) - 1);
    }
    size_t message_length = 0;
    size_t confirm_length = 0;
    size_t cancel_length = 0;
    size_t unit_length = 0;
    if (!fixed_string_length(spec->message, sizeof(spec->message), &message_length) ||
        !fixed_string_length(spec->confirm_label, sizeof(spec->confirm_label), &confirm_length) ||
        !fixed_string_length(spec->cancel_label, sizeof(spec->cancel_label), &cancel_length) ||
        !fixed_string_length(spec->price_unit, sizeof(spec->price_unit), &unit_length)) {
        return error(kQolErrInvalidArgument, kInvalidArgument, sizeof(kInvalidArgument) - 1);
    }
    const contracts::UiConfirmationSpec base{spec->text_id, spec->display_type,
        spec->format_arg, spec->price, spec->on_confirm, spec->on_cancel, spec->user};
    const contracts::QolUiAdapter* adapter = adapter_snapshot();
    if (!on_main_thread(adapter)) return not_ready();
    if (ui_panels_custom_backend_active()) {
        if (message_length == 0) {
            return error(kQolErrInvalidArgument, kInvalidArgument, sizeof(kInvalidArgument) - 1);
        }
        OverlayText text{};
        std::memcpy(text.message, spec->message, message_length + 1);
        std::memcpy(text.confirm_label, spec->confirm_label, confirm_length + 1);
        std::memcpy(text.cancel_label, spec->cancel_label, cancel_length + 1);
        std::memcpy(text.price_unit, spec->price_unit, unit_length + 1);
        text.valid = true;
        return confirmation_open_custom_v2(base, text, out_token);
    }
    if (message_length == 0) return confirmation_open(&base, out_token);
    return confirmation_open_native(&base, out_token, true, spec->message,
                                    static_cast<uint32_t>(message_length));
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
    bool custom_backend = false;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (!g_active || g_active_token != token) return stale_token();
        custom_backend = g_backend == Backend::kCustomOverlay;
        if (custom_backend) {
            *out_active = 1;
            return contracts::ok();
        }
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
    &confirmation_open_v2,
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
    if (!g_active || g_backend != Backend::kCustomOverlay || !g_installed || g_closing ||
        g_active_token == 0) {
        return false;
    }
    *out_token = g_active_token;
    return true;
}

bool ui_confirmation_modal_input_active() {
    std::lock_guard<std::mutex> lock(g_mutex);
    // The custom snapshot is the modal contract from open onward: input is consumed even
    // before the first frame draws. A failed first draw cancels the token, so no invisible
    // modal can keep consuming indefinitely.
    return g_active && g_backend == Backend::kCustomOverlay && g_active_token != 0;
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
    if (g_active && g_backend == Backend::kCustomOverlay && g_active_token == token) {
        g_overlay_drawn_once = true;
    }
}

uint32_t ui_confirmation_dispatch_custom_event(uint64_t event, uint64_t param,
                                               uint64_t param2) {
    (void)param2;
    // Modal consumption is unconditional while the custom token is active. Only the game's
    // verified touch triple reads param: 0x17 press, 0x19 move, 0x18 release. Any other
    // popup event (including bit63 system sub-events) must not be dereferenced as a point.
    const bool touch_event = event == 0x17 || event == 0x19 || event == 0x18;
    if (!touch_event) return 1;

    const contracts::QolUiAdapter* adapter = adapter_snapshot();
    uint64_t token = 0;
    qol::plugins::custom_ui::UiConfirmationOverlayLayout layout{};
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (!g_active || g_backend != Backend::kCustomOverlay || !g_overlay_drawn_once ||
            g_active_token == 0) {
            return 1;
        }
        token = g_active_token;
        if (g_overlay_layout_token != token || g_overlay_layout.valid == 0) {
            if (event == 0x18 || event == 0x17) {
                g_pressed_hit = qol::plugins::custom_ui::UiConfirmationOverlayHit::kNone;
                g_pressed_token = 0;
            }
            return 1;
        }
        layout = g_overlay_layout;
    }
    int64_t x = -1;
    int64_t y = -1;
    if (adapter == nullptr || adapter->touch_coordinates == nullptr ||
        !contracts::succeeded(adapter->touch_coordinates(param, &x, &y)) ||
        x < 0 || y < 0 || x > INT32_MAX || y > INT32_MAX) {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (event == 0x18 || event == 0x17) {
            g_pressed_hit = qol::plugins::custom_ui::UiConfirmationOverlayHit::kNone;
            g_pressed_token = 0;
        }
        return 1;
    }

    const auto hit = qol::plugins::custom_ui::ui_confirmation_overlay_hit_test(
        layout, static_cast<int32_t>(x), static_cast<int32_t>(y));
    if (event == 0x17) {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (g_active && g_active_token == token && g_backend == Backend::kCustomOverlay) {
            g_pressed_hit = hit;
            g_pressed_token = hit == qol::plugins::custom_ui::UiConfirmationOverlayHit::kNone
                ? 0 : token;
        }
        return 1;
    }
    if (event == 0x19) {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (g_pressed_token == token && g_pressed_hit != hit) {
            g_pressed_hit = qol::plugins::custom_ui::UiConfirmationOverlayHit::kNone;
            g_pressed_token = 0;
        }
        return 1;
    }
    if (event == 0x18) {
        qol::plugins::custom_ui::UiConfirmationOverlayHit pressed =
            qol::plugins::custom_ui::UiConfirmationOverlayHit::kNone;
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            if (g_pressed_token == token) pressed = g_pressed_hit;
            g_pressed_hit = qol::plugins::custom_ui::UiConfirmationOverlayHit::kNone;
            g_pressed_token = 0;
        }
        if (pressed != qol::plugins::custom_ui::UiConfirmationOverlayHit::kNone &&
            pressed == hit) {
            confirmation_dispatch_custom(
                pressed == qol::plugins::custom_ui::UiConfirmationOverlayHit::kConfirm);
        }
    }
    return 1;
}

void ui_confirmation_overlay_scope_finished(uint64_t token) {
    bool cancel_unseen = false;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        cancel_unseen = g_active && g_backend == Backend::kCustomOverlay &&
            g_active_token == token && !g_overlay_drawn_once;
    }
    if (cancel_unseen) confirmation_dispatch_custom(false);
}

bool ui_confirmation_cancel_for_return() {
    bool custom_backend = false;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        custom_backend = g_active && g_active_token != 0 &&
            g_backend == Backend::kCustomOverlay;
    }
    if (custom_backend) {
        confirmation_dispatch_custom(false);
        return true;
    }
    if (!ui_confirmation_modal_input_active()) return false;
    const contracts::QolUiAdapter* adapter = adapter_snapshot();
    uint64_t token = 0;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (!g_active || !g_overlay_drawn_once) return false;
        token = g_active_token;
    }
    if (!on_main_thread(adapter)) return true;

    uint8_t popup_active = 0;
    NativeCallback current_ok = nullptr;
    NativeCallback current_cancel = nullptr;
    if (!read_popup(adapter, &popup_active, &current_ok, &current_cancel)) return true;
    if (popup_active != 0 && current_ok == &confirmation_ok_trampoline &&
        current_cancel == &confirmation_cancel_trampoline) {
        uint8_t closed = 0;
        const QolResult result = adapter->popup_msg_close_if_callbacks(
            &confirmation_ok_trampoline, &confirmation_cancel_trampoline, &closed);
        if (!contracts::succeeded(result) || closed == 0) return true;
        bool still_active = false;
        {
        std::lock_guard<std::mutex> lock(g_mutex);
        still_active = g_active && g_active_token == token;
        if (!still_active) g_return_suppression_pending = false;
        }
        if (still_active) {
            uint8_t still_popup_active = 1;
            if (adapter->popup_msg_active(&still_popup_active).ok == kQolOk &&
                still_popup_active == 0) {
                confirmation_dispatch(false);
            }
        }
        return true;
    }
    confirmation_dispatch(false);
    return true;
}

}  // namespace qol::features::core_ui
