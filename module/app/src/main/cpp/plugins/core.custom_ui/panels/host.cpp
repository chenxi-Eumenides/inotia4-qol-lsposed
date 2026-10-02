#include "host.h"

#include <cstring>

#include "../../../contracts/ui.h"

namespace qol::features::core_ui {
namespace {

PopupHost* g_dispatch_host = nullptr;

bool sdk_popup_resolve(void* context, const char* symbol, contracts::QolPopupStateEntry* entry) {
    const auto* sdk = static_cast<const contracts::QolHostSdk*>(context);
    return sdk != nullptr && sdk->hooks.popup_resolve != nullptr &&
        contracts::succeeded(sdk->hooks.popup_resolve(symbol, entry));
}
bool sdk_popup_acquire(void* context, contracts::QolPopupStateEntry* entry,
                       const contracts::QolPopupStateHooks* hooks) {
    const auto* sdk = static_cast<const contracts::QolHostSdk*>(context);
    return sdk != nullptr && sdk->hooks.popup_acquire != nullptr &&
        contracts::succeeded(sdk->hooks.popup_acquire(entry, hooks));
}
bool sdk_popup_restore(void* context, contracts::QolPopupStateEntry* entry) {
    const auto* sdk = static_cast<const contracts::QolHostSdk*>(context);
    return sdk != nullptr && sdk->hooks.popup_restore != nullptr &&
        contracts::succeeded(sdk->hooks.popup_restore(entry));
}
bool sdk_set_process_info(void* context, int32_t process, int32_t state_id) {
    const auto* sdk = static_cast<const contracts::QolHostSdk*>(context);
    return sdk != nullptr && sdk->ui != nullptr && sdk->ui->set_popup_process_info != nullptr &&
        contracts::succeeded(sdk->ui->set_popup_process_info(process, state_id));
}

void dispatch_enter() { if (g_dispatch_host != nullptr) g_dispatch_host->dispatch_enter(); }
void dispatch_process() { if (g_dispatch_host != nullptr) g_dispatch_host->dispatch_process(); }
void dispatch_f3() { if (g_dispatch_host != nullptr) g_dispatch_host->dispatch_f3(); }
void dispatch_f4() { if (g_dispatch_host != nullptr) g_dispatch_host->dispatch_f4(); }
uint32_t dispatch_event(uint64_t a, uint64_t b, uint64_t c) {
    return g_dispatch_host == nullptr ? 0 : g_dispatch_host->dispatch_event(a, b, c);
}

const contracts::QolPopupStateHooks kDispatchers{
    dispatch_enter, dispatch_process, dispatch_f3, dispatch_f4, dispatch_event};

bool has_callbacks(const contracts::PanelProviderSpec& provider) {
    const auto* callbacks = provider.callbacks;
    return callbacks != nullptr && (callbacks->enter != nullptr || callbacks->process != nullptr ||
        callbacks->f3 != nullptr || callbacks->f4 != nullptr || callbacks->event != nullptr);
}

}  // namespace

PopupHost::PopupHost(const char* popup_symbol, PopupHostHooks hooks)
    : popup_symbol_(popup_symbol), hooks_(hooks) {
    g_dispatch_host = this;
    popup_entry_.state_id = -1;
}

PopupHost::PopupHost(const char* popup_symbol, const contracts::QolHostSdk* sdk)
    : PopupHost(popup_symbol, {const_cast<contracts::QolHostSdk*>(sdk), sdk_popup_resolve,
                               sdk_popup_acquire, sdk_popup_restore, sdk_set_process_info}) {}

PopupHost::~PopupHost() {
    if (g_dispatch_host == this) g_dispatch_host = nullptr;
}

bool PopupHost::ensure_popup(const contracts::PanelProviderSpec& provider) {
    if (acquired_) return true;
    if (!has_callbacks(provider)) return true;
    if (popup_symbol_ == nullptr || hooks_.resolve == nullptr || hooks_.acquire == nullptr ||
        hooks_.restore == nullptr || hooks_.set_process_info == nullptr) return false;
    popup_entry_ = {};
    popup_entry_.state_id = -1;
    if (!hooks_.resolve(hooks_.context, popup_symbol_, &popup_entry_) || popup_entry_.slot == nullptr ||
        !hooks_.acquire(hooks_.context, &popup_entry_, &kDispatchers)) {
        popup_entry_ = {};
        popup_entry_.state_id = -1;
        return false;
    }
    acquired_ = true;
    return true;
}

bool PopupHost::open(const contracts::PanelProviderSpec& provider, bool main_thread) {
    main_thread_ = main_thread;
    if (!main_thread || deactivating_ || provider.panel_id == nullptr || provider.open == nullptr ||
        provider.close == nullptr || close_pending_) return false;
    if (stack_size_ > 0 && std::strcmp(stack_[stack_size_ - 1].provider->panel_id,
                                       provider.panel_id) == 0) {
        return stack_[stack_size_ - 1].provider->active == nullptr ||
               stack_[stack_size_ - 1].provider->active();
    }
    if (stack_size_ >= kStackCapacity || !ensure_popup(provider)) return false;

    const contracts::PanelProviderSpec* previous = stack_size_ == 0 ? nullptr : stack_[stack_size_ - 1].provider;
    if (previous != nullptr) {
        if (previous->close != nullptr) previous->close();
        if (has_callbacks(*previous) &&
            !hooks_.set_process_info(hooks_.context, 3, 0)) return false;
    }
    stack_[stack_size_++].provider = &provider;
    if (!provider.open()) {
        if (provider.active != nullptr && provider.active()) provider.close();
        --stack_size_;
        stack_[stack_size_] = {};
        if (previous != nullptr) {
            if (previous->open != nullptr) (void)previous->open();
            if (has_callbacks(*previous)) {
                (void)hooks_.set_process_info(hooks_.context, 1, popup_entry_.state_id);
            }
        }
        if (stack_size_ == 0 && acquired_) {
            close_pending_ = true;
            pending_provider_ = &provider;
        }
        restore_if_idle();
        return false;
    }
    if (has_callbacks(provider) &&
        !hooks_.set_process_info(hooks_.context, 1, popup_entry_.state_id)) {
        if (provider.close != nullptr) provider.close();
        --stack_size_;
        stack_[stack_size_] = {};
        if (previous != nullptr) {
            stack_[stack_size_++].provider = previous;
            if (previous->open != nullptr) (void)previous->open();
            if (has_callbacks(*previous)) {
                (void)hooks_.set_process_info(hooks_.context, 1, popup_entry_.state_id);
            }
        } else if (acquired_) {
            close_pending_ = true;
            pending_provider_ = &provider;
        }
        restore_if_idle();
        return false;
    }
    return true;
}

bool PopupHost::close_top() {
    if (stack_size_ == 0) return false;
    const contracts::PanelProviderSpec* previous = stack_[stack_size_ - 1].provider;
    if (previous->close != nullptr) previous->close();
    if (has_callbacks(*previous) &&
        !hooks_.set_process_info(hooks_.context, 3, 0)) return false;
    stack_[--stack_size_] = {};
    if (stack_size_ > 0) {
        const contracts::PanelProviderSpec* restored = stack_[stack_size_ - 1].provider;
        if (restored->open != nullptr && !restored->open()) return false;
        if (has_callbacks(*restored) &&
            !hooks_.set_process_info(hooks_.context, 1, popup_entry_.state_id)) return false;
    } else if (acquired_) {
        close_pending_ = true;
        pending_provider_ = previous;
    }
    return true;
}

bool PopupHost::owns_callbacks(const contracts::PanelProviderSpec& provider) const {
    return has_callbacks(provider);
}

bool PopupHost::close(const char* panel_id, bool main_thread) {
    main_thread_ = main_thread;
    if (!main_thread || panel_id == nullptr || stack_size_ == 0 || deactivating_) return false;
    if (std::strcmp(stack_[stack_size_ - 1].provider->panel_id, panel_id) != 0) return false;
    return close_top();
}

void PopupHost::deactivate(bool main_thread) {
    main_thread_ = main_thread;
    if (!main_thread || deactivating_) return;
    deactivating_ = true;
    while (stack_size_ > 0) {
        const contracts::PanelProviderSpec* provider = stack_[--stack_size_].provider;
        stack_[stack_size_] = {};
        if (provider != nullptr && provider->close != nullptr) provider->close();
        if (provider != nullptr && has_callbacks(*provider)) {
            (void)hooks_.set_process_info(hooks_.context, 3, 0);
        }
    }
    if (acquired_) {
        close_pending_ = true;
        pending_provider_ = nullptr;
    }
    restore_if_idle();
}

bool PopupHost::pump(bool main_thread) {
    main_thread_ = main_thread;
    if (!main_thread) return false;
    restore_if_idle();
    return true;
}

void PopupHost::restore_if_idle() {
    if (!close_pending_ || dispatch_depth_ != 0) return;
    if (stack_size_ != 0) return;
    if (pending_provider_ != nullptr && pending_provider_->active != nullptr &&
        pending_provider_->active()) return;
    if (popup_entry_.slot != nullptr && hooks_.restore != nullptr && hooks_.restore(hooks_.context, &popup_entry_)) {
        acquired_ = false;
        close_pending_ = false;
        pending_provider_ = nullptr;
        popup_entry_ = {};
        popup_entry_.state_id = -1;
    }
}

const contracts::PanelCallbacks* PopupHost::current_callbacks() const {
    return stack_size_ == 0 ? nullptr : stack_[stack_size_ - 1].provider->callbacks;
}

void PopupHost::dispatch_enter() {
    if (deactivating_ || !main_thread_) return;
    ++dispatch_depth_;
    const auto* cb = current_callbacks(); if (cb != nullptr && cb->enter != nullptr) cb->enter();
    --dispatch_depth_;
    restore_if_idle();
}
void PopupHost::dispatch_process() {
    if (deactivating_ || !main_thread_) return;
    ++dispatch_depth_;
    const auto* cb = current_callbacks(); if (cb != nullptr && cb->process != nullptr) cb->process();
    --dispatch_depth_;
    restore_if_idle();
}
void PopupHost::dispatch_f3() {
    if (deactivating_ || !main_thread_) return;
    ++dispatch_depth_;
    const auto* cb = current_callbacks(); if (cb != nullptr && cb->f3 != nullptr) cb->f3();
    --dispatch_depth_;
    if (stack_size_ > 0) {
        const auto* provider = stack_[stack_size_ - 1].provider;
        if (provider->active != nullptr && !provider->active()) (void)close_top();
    }
    restore_if_idle();
}
void PopupHost::dispatch_f4() {
    if (deactivating_ || !main_thread_) return;
    ++dispatch_depth_;
    const auto* cb = current_callbacks(); if (cb != nullptr && cb->f4 != nullptr) cb->f4();
    --dispatch_depth_;
    restore_if_idle();
}
uint32_t PopupHost::dispatch_event(uint64_t a, uint64_t b, uint64_t c) {
    if (deactivating_ || !main_thread_) return 0;
    ++dispatch_depth_;
    const auto* cb = current_callbacks();
    const uint32_t result = cb != nullptr && cb->event != nullptr ? cb->event(a, b, c) : 0;
    --dispatch_depth_;
    restore_if_idle();
    return result;
}

const contracts::QolPopupStateHooks* PopupHost::dispatchers_for_test() const { return &kDispatchers; }
const char* PopupHost::current_panel_for_test() const {
    return stack_size_ == 0 ? nullptr : stack_[stack_size_ - 1].provider->panel_id;
}
bool PopupHost::close_pending_for_test() const { return close_pending_; }
bool PopupHost::active_for_test() const { return stack_size_ != 0; }

}  // namespace qol::features::core_ui
