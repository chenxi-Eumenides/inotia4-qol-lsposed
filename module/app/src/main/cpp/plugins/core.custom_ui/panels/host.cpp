#include "host.h"

#include <cstdarg>
#include <cstdio>
#include <cstring>

#include "../../../contracts/ui.h"

namespace qol::features::core_ui {
namespace {

PopupHost* g_dispatch_host = nullptr;
void sdk_log(void* context, int32_t level, const char* message);

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
bool sdk_popup_stack_snapshot(void* context, uint32_t* count, uintptr_t* top_enter_vma) {
    const auto* sdk = static_cast<const contracts::QolHostSdk*>(context);
    if (sdk == nullptr || sdk->ui == nullptr || sdk->ui->popup_stack_snapshot == nullptr) return false;
    const QolResult result = sdk->ui->popup_stack_snapshot(count, top_enter_vma);
    if (!contracts::succeeded(result) && result.message != nullptr) {
        char message[320]{};
        std::snprintf(message, sizeof(message), "snapshot seam game=%.*s",
                      static_cast<int>(result.message_len), result.message);
        sdk_log(context, 1, message);
    }
    return contracts::succeeded(result);
}
void sdk_log(void* context, int32_t level, const char* message) {
    const auto* sdk = static_cast<const contracts::QolHostSdk*>(context);
    if (sdk == nullptr || sdk->log.write == nullptr || message == nullptr) return;
    (void)sdk->log.write(level, 12, "core.custom_ui/panels/host", message,
                         static_cast<uint32_t>(std::strlen(message)));
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
                               sdk_popup_acquire, sdk_popup_restore, sdk_set_process_info,
                               sdk_popup_stack_snapshot, sdk_log}) {}

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
    const bool resolved = hooks_.resolve(hooks_.context, popup_symbol_, &popup_entry_) &&
                          popup_entry_.slot != nullptr;
    if (!resolved) {
        log_message("ensure_popup resolve=failed");
        popup_entry_ = {};
        popup_entry_.state_id = -1;
        return false;
    }
    const bool acquired = hooks_.acquire(hooks_.context, &popup_entry_, &kDispatchers);
    log_message(acquired ? "ensure_popup resolve=ok acquire=ok" :
                            "ensure_popup resolve=ok acquire=failed");
    if (!acquired) {
        popup_entry_ = {};
        popup_entry_.state_id = -1;
        return false;
    }
    acquired_ = true;
    return true;
}

bool PopupHost::open(const contracts::PanelProviderSpec& provider, bool main_thread) {
    main_thread_ = main_thread;
    if (last_open_main_thread_ != main_thread || last_open_deactivating_ != deactivating_ ||
        last_open_close_pending_ != close_pending_ || last_open_stack_size_ != stack_size_) {
        char message[144]{};
        std::snprintf(message, sizeof(message),
                      "open conditions main_thread=%d deactivating=%d close_pending=%d stack_size=%d",
                      main_thread ? 1 : 0, deactivating_ ? 1 : 0, close_pending_ ? 1 : 0,
                      stack_size_);
        log_message(message);
        last_open_main_thread_ = main_thread;
        last_open_deactivating_ = deactivating_;
        last_open_close_pending_ = close_pending_;
        last_open_stack_size_ = stack_size_;
    }
    if (!main_thread || deactivating_ || provider.panel_id == nullptr || provider.open == nullptr ||
        provider.close == nullptr || close_pending_) return false;
    if (stack_size_ > 0 && std::strcmp(stack_[stack_size_ - 1].provider->panel_id,
                                       provider.panel_id) == 0) {
        return stack_[stack_size_ - 1].provider->active == nullptr ||
               stack_[stack_size_ - 1].provider->active();
    }
    if (stack_size_ >= kStackCapacity) return false;
    if (!acquired_) {
        original_popup_snapshot_valid_ = snapshot(&original_popup_count_, &original_popup_top_vma_);
        if (!original_popup_snapshot_valid_) return false;
        log_snapshot("open-baseline", original_popup_count_, original_popup_top_vma_);
    }
    if (!ensure_popup(provider)) {
        if (!acquired_) original_popup_snapshot_valid_ = false;
        return false;
    }

    const contracts::PanelProviderSpec* previous = stack_size_ == 0 ? nullptr : stack_[stack_size_ - 1].provider;
    stack_[stack_size_++].provider = &provider;
    if (!provider.open()) {
        if (provider.active != nullptr && provider.active()) provider.close();
        --stack_size_;
        stack_[stack_size_] = {};
        if (previous != nullptr && previous->open != nullptr) (void)previous->open();
        if (stack_size_ == 0 && acquired_) {
            close_pending_ = true;
            pending_provider_ = &provider;
        }
        restore_if_idle();
        return false;
    }
    if (stack_size_ == 1 && has_callbacks(provider) &&
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
    // 嵌套：共享的原始 popup 不转换，手动进入新顶层，父面板保持原状。
    if (stack_size_ > 1 && has_callbacks(provider)) invoke_enter(provider);
    if (has_callbacks(provider) && !capture_owned_popup()) {
        provider.close();
        --stack_size_;
        stack_[stack_size_] = {};
        close_pending_ = true;
        pending_provider_ = &provider;
        log_message("owned popup snapshot unavailable; restore held pending");
        return false;
    }
    return true;
}

bool PopupHost::close_top() {
    if (stack_size_ == 0) return false;
    const contracts::PanelProviderSpec* previous = stack_[stack_size_ - 1].provider;
    uint32_t current_count = 0;
    uintptr_t current_top_vma = 0;
    if (has_callbacks(*previous) && original_popup_snapshot_valid_ && stack_size_ == 1 &&
        (!snapshot(&current_count, &current_top_vma) || !owned_popup_snapshot_valid_ ||
         current_count != owned_popup_count_ || current_top_vma != owned_popup_top_vma_)) {
        close_pending_ = true;
        retry_close_top_ = true;
        pending_provider_ = previous;
        log_message("close queued panel=%s snapshot_valid=%d owned_valid=%d current=%u/0x%lx owned=%u/0x%lx",
                    previous->panel_id, original_popup_snapshot_valid_ ? 1 : 0,
                    owned_popup_snapshot_valid_ ? 1 : 0, current_count,
                    static_cast<unsigned long>(current_top_vma), owned_popup_count_,
                    static_cast<unsigned long>(owned_popup_top_vma_));
        return true;
    }
    if (previous->close != nullptr) previous->close();
    stack_[--stack_size_] = {};
    if (stack_size_ > 0) {
        const contracts::PanelProviderSpec* restored = stack_[stack_size_ - 1].provider;
        if (restored->open != nullptr && !restored->open()) return false;
        if (has_callbacks(*restored)) (void)capture_owned_popup();
        // 逻辑回到父面板：不 pop 游戏栈，手动重新进入父面板刷新其状态。
        if (has_callbacks(*restored)) invoke_enter(*restored);
    } else if (acquired_) {
        close_pending_ = true;
        pending_provider_ = previous;
        idle_baseline_pumps_ = 0;
    }
    return true;
}

bool PopupHost::owns_callbacks(const contracts::PanelProviderSpec& provider) const {
    return has_callbacks(provider);
}

bool PopupHost::top_is(const char* panel_id) const {
    return stack_size_ > 0 && panel_id != nullptr &&
           stack_[stack_size_ - 1].provider != nullptr &&
           stack_[stack_size_ - 1].provider->panel_id != nullptr &&
           std::strcmp(stack_[stack_size_ - 1].provider->panel_id, panel_id) == 0;
}

bool PopupHost::close(const char* panel_id, bool main_thread) {
    main_thread_ = main_thread;
    if (!main_thread || panel_id == nullptr || stack_size_ == 0 || deactivating_) return false;
    if (std::strcmp(stack_[stack_size_ - 1].provider->panel_id, panel_id) != 0) return false;
    return close_top();
}

bool PopupHost::deactivate_provider(const char* owner, bool main_thread) {
    main_thread_ = main_thread;
    if (!main_thread || owner == nullptr || owner[0] == '\0' || deactivating_) return false;

    int first_index = -1;
    bool removes_top = false;
    for (int i = 0; i < stack_size_; ++i) {
        const contracts::PanelProviderSpec* provider = stack_[i].provider;
        if (provider != nullptr && provider->owner != nullptr &&
            std::strcmp(provider->owner, owner) == 0) {
            if (first_index < 0) first_index = i;
            removes_top = i == stack_size_ - 1;
        }
    }
    if (first_index < 0) {
        restore_if_idle();
        return !close_pending_;
    }

    const contracts::PanelProviderSpec* removed = stack_[stack_size_ - 1].provider;
    if (removes_top && stack_size_ == 1 && has_callbacks(*removed) && original_popup_snapshot_valid_) {
        uint32_t current_count = 0;
        uintptr_t current_top_vma = 0;
        if (!snapshot(&current_count, &current_top_vma) || !owned_popup_snapshot_valid_ ||
            current_count != owned_popup_count_ || current_top_vma != owned_popup_top_vma_) {
            close_pending_ = true;
            pending_deactivate_owner_ = owner;
            pending_provider_ = removed;
            log_message("deactivate queued snapshot unavailable owner=%s panel=%s",
                        owner, removed->panel_id);
            return true;
        }
    }
    if (removes_top && removed->close != nullptr && removed->active != nullptr && removed->active()) {
        removed->close();
    }

    for (int i = 0; i < stack_size_;) {
        const contracts::PanelProviderSpec* provider = stack_[i].provider;
        if (provider != nullptr && provider->owner != nullptr &&
            std::strcmp(provider->owner, owner) == 0) {
            for (int j = i; j + 1 < stack_size_; ++j) stack_[j] = stack_[j + 1];
            stack_[--stack_size_] = {};
        } else {
            ++i;
        }
    }

    if (removes_top && stack_size_ > 0) {
        const contracts::PanelProviderSpec* restored = stack_[stack_size_ - 1].provider;
        if (restored->open != nullptr && !restored->open()) return false;
        if (has_callbacks(*restored)) (void)capture_owned_popup();
        if (has_callbacks(*restored)) invoke_enter(*restored);
        return true;
    }
    if (removes_top && acquired_) {
        close_pending_ = true;
        pending_provider_ = removed;
        idle_baseline_pumps_ = 0;
        return true;
    }
    restore_if_idle();
    return true;
}

void PopupHost::deactivate(bool main_thread) {
    main_thread_ = main_thread;
    if (!main_thread || deactivating_) return;
    deactivating_ = true;
    while (stack_size_ > 0) {
        const contracts::PanelProviderSpec* provider = stack_[--stack_size_].provider;
        stack_[stack_size_] = {};
        if (provider != nullptr && provider->close != nullptr) provider->close();
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
    if (pending_deactivate_owner_ != nullptr) {
        const char* owner = pending_deactivate_owner_;
        pending_deactivate_owner_ = nullptr;
        close_pending_ = false;
        pending_provider_ = nullptr;
        (void)deactivate_provider(owner, true);
    } else if (retry_close_top_) {
        retry_close_top_ = false;
        close_pending_ = false;
        pending_provider_ = nullptr;
        (void)close_top();
    }
    pumping_ = true;
    restore_if_idle();
    pumping_ = false;
    track_close_stall();
    return true;
}

void PopupHost::track_close_stall() {
    const bool close_in_flight = close_pending_ || retry_close_top_;
    if (!close_in_flight) {
        close_stall_frames_ = 0;
        close_stall_progress_ = CloseProgress{};
        return;
    }
    const CloseProgress current{stack_size_, idle_baseline_pumps_, close_pending_,
                                retry_close_top_, pop_issued_};
    if (current.differs_from(close_stall_progress_)) {
        // 任一可观察进度变化（关闭完成、pop 发出、基线确认帧推进等）→ 重计停滞。
        close_stall_progress_ = current;
        close_stall_frames_ = 0;
        return;
    }
    if (++close_stall_frames_ < kCloseStallFrameLimit) return;
    force_complete_close();
}

void PopupHost::force_complete_close() {
    const contracts::PanelProviderSpec* pending = pending_provider_;
    char panel_name[64] = "none";
    if (pending != nullptr && pending->panel_id != nullptr) {
        std::snprintf(panel_name, sizeof(panel_name), "%s", pending->panel_id);
    }
    // 内部栈残留的 provider 一律补 close（close 约定幂等）；此时游戏栈状态不可信，
    // 不能再等快照一致。
    while (stack_size_ > 0) {
        const contracts::PanelProviderSpec* provider = stack_[--stack_size_].provider;
        stack_[stack_size_] = {};
        if (provider != nullptr && provider->close != nullptr) provider->close();
    }
    // pop 只补发一次：restore 阶段已发过的（pop_issued_）不重复，避免游戏栈被多弹一层。
    const bool popped = pop_issued_ || (hooks_.set_process_info != nullptr &&
                                        hooks_.set_process_info(hooks_.context, 3, 0));
    // popup entry 占用尽力释放（条目字段还原，不动游戏栈）；收尾后不再有 restore 流程。
    if (acquired_ && popup_entry_.slot != nullptr && hooks_.restore != nullptr) {
        (void)hooks_.restore(hooks_.context, &popup_entry_);
    }
    log_message("close force-complete panel=%s stall_frames=%u limit=%u pop=%d", panel_name,
                close_stall_frames_, kCloseStallFrameLimit, popped ? 1 : 0);
    acquired_ = false;
    close_pending_ = false;
    retry_close_top_ = false;
    original_popup_snapshot_valid_ = false;
    owned_popup_snapshot_valid_ = false;
    pop_issued_ = false;
    pending_provider_ = nullptr;
    pending_deactivate_owner_ = nullptr;
    idle_baseline_pumps_ = 0;
    close_stall_frames_ = 0;
    close_stall_progress_ = CloseProgress{};
    popup_entry_ = {};
    popup_entry_.state_id = -1;
}

void PopupHost::restore_if_idle() {
    if (!pumping_ || !close_pending_ || dispatch_depth_ != 0) return;
    if (stack_size_ != 0) return;
    if (pending_provider_ != nullptr && pending_provider_->active != nullptr &&
        pending_provider_->active()) return;
    if (!original_popup_snapshot_valid_) {
        log_message("restore held pending reason=baseline_snapshot_unavailable");
        return;
    }
    uint32_t game_count = 0;
    uintptr_t game_top_vma = 0;
    if (!snapshot(&game_count, &game_top_vma)) {
        idle_baseline_pumps_ = 0;
        return;
    }
    const bool at_original_target = game_count == original_popup_count_ &&
                                    game_top_vma == original_popup_top_vma_;
    if (at_original_target) {
        if (++idle_baseline_pumps_ < 2) return;
    } else {
        idle_baseline_pumps_ = 0;
    }
    if (!at_original_target && !pop_issued_) {
        if (!owned_popup_snapshot_valid_ || game_count != owned_popup_count_ ||
            game_top_vma != owned_popup_top_vma_) {
            log_message("restore held pending reason=game_stack_unexpected current=%u/0x%lx owned_valid=%d owned=%u/0x%lx",
                        game_count, static_cast<unsigned long>(game_top_vma),
                        owned_popup_snapshot_valid_ ? 1 : 0, owned_popup_count_,
                        static_cast<unsigned long>(owned_popup_top_vma_));
            return;
        }
        pop_issued_ = hooks_.set_process_info != nullptr &&
                      hooks_.set_process_info(hooks_.context, 3, 0);
        if (!pop_issued_) return;
        idle_baseline_pumps_ = 0;
        log_message("pop issued; restore deferred to a later frame");
        return;
    }
    if (game_count != original_popup_count_ || game_top_vma != original_popup_top_vma_ ||
        idle_baseline_pumps_ < 2) return;
    if (popup_entry_.slot != nullptr && hooks_.restore != nullptr && hooks_.restore(hooks_.context, &popup_entry_)) {
        if (hooks_.log != nullptr) {
            char message[160]{};
            std::snprintf(message, sizeof(message),
                          "restore complete internal_stack=%d game_popup_count=%u top_enter_vma=0x%lx",
                          stack_size_, game_count, static_cast<unsigned long>(game_top_vma));
            hooks_.log(hooks_.context, 1, message);
        }
        acquired_ = false;
        close_pending_ = false;
        original_popup_snapshot_valid_ = false;
        owned_popup_snapshot_valid_ = false;
        pop_issued_ = false;
        pending_provider_ = nullptr;
        idle_baseline_pumps_ = 0;
        popup_entry_ = {};
        popup_entry_.state_id = -1;
    }
}

bool PopupHost::snapshot(uint32_t* count, uintptr_t* top_enter_vma) {
    if (!main_thread_ || count == nullptr || top_enter_vma == nullptr ||
        hooks_.popup_stack_snapshot == nullptr) {
        if (!last_snapshot_log_valid_ || last_snapshot_log_ok_) {
            log_message("popup snapshot unavailable main_thread=%d hook=%d",
                        main_thread_ ? 1 : 0, hooks_.popup_stack_snapshot != nullptr ? 1 : 0);
        }
        last_snapshot_log_valid_ = true;
        last_snapshot_log_ok_ = false;
        return false;
    }
    const bool ok = hooks_.popup_stack_snapshot(hooks_.context, count, top_enter_vma);
    const uint32_t logged_count = ok ? *count : 0;
    const uintptr_t logged_top = ok ? *top_enter_vma : 0;
    if (!last_snapshot_log_valid_ || last_snapshot_log_ok_ != ok ||
        last_snapshot_log_count_ != logged_count || last_snapshot_log_top_vma_ != logged_top) {
        log_message("popup snapshot result=%s count=%u top=0x%lx", ok ? "ok" : "failed",
                    logged_count, static_cast<unsigned long>(logged_top));
    }
    last_snapshot_log_valid_ = true;
    last_snapshot_log_ok_ = ok;
    last_snapshot_log_count_ = logged_count;
    last_snapshot_log_top_vma_ = logged_top;
    return ok;
}

bool PopupHost::capture_owned_popup() {
    owned_popup_snapshot_valid_ = snapshot(&owned_popup_count_, &owned_popup_top_vma_);
    if (owned_popup_snapshot_valid_) {
        pop_issued_ = false;
        log_snapshot("owned-entry", owned_popup_count_, owned_popup_top_vma_);
    }
    return owned_popup_snapshot_valid_;
}

void PopupHost::log_pop_issued() {
    uint32_t count = original_popup_count_;
    uintptr_t top_vma = original_popup_top_vma_;
    (void)snapshot(&count, &top_vma);
    log_snapshot("pop-issued", count, top_vma);
}

void PopupHost::log_snapshot(const char* event, uint32_t count, uintptr_t top_enter_vma) {
    if (hooks_.log == nullptr || event == nullptr) return;
    if (count == last_logged_popup_count_ && top_enter_vma == last_logged_popup_top_vma_ &&
        std::strcmp(event, "open-baseline") != 0 && std::strcmp(event, "pop-issued") != 0) return;
    last_logged_popup_count_ = count;
    last_logged_popup_top_vma_ = top_enter_vma;
    char message[192]{};
    std::snprintf(message, sizeof(message),
                  "%s internal_stack=%d game_popup_count=%u top_enter_vma=0x%lx target_count=%u target_top_vma=0x%lx",
                  event, stack_size_, count, static_cast<unsigned long>(top_enter_vma),
                  original_popup_count_, static_cast<unsigned long>(original_popup_top_vma_));
    hooks_.log(hooks_.context, 1, message);
}

void PopupHost::log_message(const char* format, ...) {
    if (hooks_.log == nullptr || format == nullptr) return;
    char message[192]{};
    va_list args;
    va_start(args, format);
    std::vsnprintf(message, sizeof(message), format, args);
    va_end(args);
    hooks_.log(hooks_.context, 1, message);
}

const contracts::PanelCallbacks* PopupHost::current_callbacks() const {
    return stack_size_ == 0 ? nullptr : stack_[stack_size_ - 1].provider->callbacks;
}

void PopupHost::invoke_enter(const contracts::PanelProviderSpec& provider) {
    const auto* cb = provider.callbacks;
    if (cb == nullptr || cb->enter == nullptr) return;
    ++dispatch_depth_;
    cb->enter();
    --dispatch_depth_;
}

void PopupHost::dispatch_enter() {
    if (deactivating_ || !main_thread_) return;
    log_message("dispatch enter panel=%s main_thread=%d count=%u",
                current_panel_for_test() == nullptr ? "none" : current_panel_for_test(),
                main_thread_ ? 1 : 0, process_dispatch_count_);
    ++dispatch_depth_;
    if (stack_size_ > 0 && !original_popup_snapshot_valid_) {
        uint32_t baseline_count = 0;
        uintptr_t baseline_top_vma = 0;
        if (snapshot(&baseline_count, &baseline_top_vma)) {
            original_popup_count_ = baseline_count;
            original_popup_top_vma_ = baseline_top_vma;
            original_popup_snapshot_valid_ = true;
            log_snapshot("open-baseline-recovered", baseline_count, baseline_top_vma);
        }
    }
    if (stack_size_ > 0 && current_callbacks() != nullptr && current_callbacks()->enter != nullptr) {
        (void)capture_owned_popup();
    }
    const auto* cb = current_callbacks(); if (cb != nullptr && cb->enter != nullptr) cb->enter();
    --dispatch_depth_;
    restore_if_idle();
}
void PopupHost::dispatch_process() {
    if (deactivating_ || !main_thread_) return;
    if (stack_size_ > 0 && current_callbacks() != nullptr && original_popup_snapshot_valid_ &&
        owned_popup_snapshot_valid_ && owned_popup_count_ == original_popup_count_ &&
        owned_popup_top_vma_ == original_popup_top_vma_) {
        uint32_t current_count = 0;
        uintptr_t current_top_vma = 0;
        if (snapshot(&current_count, &current_top_vma) &&
            (current_count != original_popup_count_ ||
             current_top_vma != original_popup_top_vma_)) {
            owned_popup_count_ = current_count;
            owned_popup_top_vma_ = current_top_vma;
            pop_issued_ = false;
            log_snapshot("owned-entry", owned_popup_count_, owned_popup_top_vma_);
        }
    }
    ++process_dispatch_count_;
    if (process_dispatch_count_ == 1 || process_dispatch_count_ % 120u == 0) {
        log_message("dispatch process panel=%s main_thread=%d count=%u",
                    current_panel_for_test() == nullptr ? "none" : current_panel_for_test(),
                    main_thread_ ? 1 : 0, process_dispatch_count_);
    }
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
bool PopupHost::pending_deactivate_for_owner(const char* owner) const {
    return owner != nullptr && pending_deactivate_owner_ != nullptr &&
           std::strcmp(owner, pending_deactivate_owner_) == 0;
}
bool PopupHost::active_for_test() const { return stack_size_ != 0; }

}  // namespace qol::features::core_ui
