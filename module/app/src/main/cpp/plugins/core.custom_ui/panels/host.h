#pragma once

#include <cstdint>

#include "../../../contracts/host_sdk.h"
#include "../../../contracts/panels.h"

namespace qol::features::core_ui {

struct PopupHostHooks {
    void* context;
    bool (*resolve)(void* context, const char* symbol, contracts::QolPopupStateEntry* entry);
    bool (*acquire)(void* context, contracts::QolPopupStateEntry* entry,
                    const contracts::QolPopupStateHooks* hooks);
    bool (*restore)(void* context, contracts::QolPopupStateEntry* entry);
    bool (*set_process_info)(void* context, int32_t process, int32_t state_id);
    bool (*popup_stack_snapshot)(void* context, uint32_t* count, uintptr_t* top_enter_vma);
    void (*log)(void* context, int32_t level, const char* message);
};

class PopupHost {
public:
    static constexpr int kStackCapacity = 8;

    PopupHost(const char* popup_symbol, PopupHostHooks hooks);
    PopupHost(const char* popup_symbol, const contracts::QolHostSdk* sdk);
    ~PopupHost();
    PopupHost(const PopupHost&) = delete;
    PopupHost& operator=(const PopupHost&) = delete;

    bool open(const contracts::PanelProviderSpec& provider, bool main_thread);
    bool close(const char* panel_id, bool main_thread);
    bool deactivate_provider(const char* owner, bool main_thread);
    bool owns_callbacks(const contracts::PanelProviderSpec& provider) const;
    void deactivate(bool main_thread);
    // Runs outside popup callbacks; restores only after the current provider is inactive.
    bool pump(bool main_thread);

    const contracts::QolPopupStateHooks* dispatchers_for_test() const;
    const char* current_panel_for_test() const;
    bool close_pending_for_test() const;
    bool active_for_test() const;
    void dispatch_enter();
    void dispatch_process();
    void dispatch_f3();
    void dispatch_f4();
    uint32_t dispatch_event(uint64_t a, uint64_t b, uint64_t c);

private:
    struct StackItem {
        const contracts::PanelProviderSpec* provider = nullptr;
    };

    bool ensure_popup(const contracts::PanelProviderSpec& provider);
    bool snapshot(uint32_t* count, uintptr_t* top_enter_vma);
    bool capture_owned_popup();
    void log_pop_issued();
    void log_snapshot(const char* event, uint32_t count, uintptr_t top_enter_vma);
    const contracts::PanelCallbacks* current_callbacks() const;
    bool close_top();
    void restore_if_idle();

    const char* popup_symbol_;
    PopupHostHooks hooks_;
    contracts::QolPopupStateEntry popup_entry_{};
    const contracts::PanelProviderSpec* pending_provider_ = nullptr;
    StackItem stack_[kStackCapacity]{};
    int stack_size_ = 0;
    uint32_t dispatch_depth_ = 0;
    uint32_t original_popup_count_ = 0;
    uintptr_t original_popup_top_vma_ = 0;
    uint32_t owned_popup_count_ = 0;
    uintptr_t owned_popup_top_vma_ = 0;
    uint32_t last_logged_popup_count_ = UINT32_MAX;
    uintptr_t last_logged_popup_top_vma_ = UINTPTR_MAX;
    bool acquired_ = false;
    bool close_pending_ = false;
    bool original_popup_snapshot_valid_ = false;
    bool owned_popup_snapshot_valid_ = false;
    bool pop_issued_ = false;
    bool deactivating_ = false;
    bool main_thread_ = false;
};

}  // namespace qol::features::core_ui
