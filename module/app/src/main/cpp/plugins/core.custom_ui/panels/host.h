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
    bool (*begin_overlay_scope)(void* context, uint64_t token, const void* provider,
                                uint64_t generation);
    void (*end_overlay_scope)(void* context);
    // Optional proactive LCD snapshot (GRP_SaveLCD) after a new provider enters the stack.
    void (*save_lcd)(void* context);
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

    // 宿主内部栈顶是否仍是该面板：close() 返回 true 但栈顶未变 = 延迟关闭已排队
    // （快照守卫），面板尚未真正关闭；调用方（panels/stack）据此不提前弹自己的栈记录。
    bool top_is(const char* panel_id) const;

    // 延迟关闭/还原停滞的逃生上限（按帧泵帧数计）：快照持续失配时守卫会无限重排队，
    // 停滞连续超过该帧数即强制走完关闭（关 provider + 补发一次 pop + 清空 close 状态机）。
    static constexpr uint32_t kCloseStallFrameLimit = 120;

    const contracts::QolPopupStateHooks* dispatchers_for_test() const;
    const char* current_panel_for_test() const;
    bool close_pending_for_test() const;
    bool pending_deactivate_for_owner(const char* owner) const;
    bool active_for_test() const;
    uint64_t top_open_generation_for_test() const;
    bool overlay_scope_matches(const void* provider, uint64_t generation) const;
    bool custom_backend_active_for_confirmation() const;
    bool custom_overlay_scope_identity(const void** provider, uint64_t* generation) const;
    // 快照中是否存在原生面板基线（打开模块面板前游戏 popup 栈非空）。原生面板之上打开
    // 模块面板时，nav 的 Auto 模式据此显示返回箭头而不是关闭 X。
    bool has_original_popup_baseline() const;
    void dispatch_enter();
    void dispatch_process();
    void dispatch_f3();
    void dispatch_f4();
    uint32_t dispatch_event(uint64_t a, uint64_t b, uint64_t c);

private:
    struct StackItem {
        const contracts::PanelProviderSpec* provider = nullptr;
        uint64_t open_generation = 0;
    };

    bool ensure_popup(const contracts::PanelProviderSpec& provider);
    bool snapshot(uint32_t* count, uintptr_t* top_enter_vma);
    bool capture_owned_popup();
    void log_pop_issued();
    void log_snapshot(const char* event, uint32_t count, uintptr_t top_enter_vma);
    void log_message(const char* format, ...);
    const contracts::PanelCallbacks* current_callbacks() const;
    bool close_top();
    void restore_if_idle();
    // 嵌套逻辑切换：不触发游戏 popup 转换，手动调用新顶层的 enter 回调。
    void invoke_enter(const contracts::PanelProviderSpec& provider);
    uint64_t allocate_open_generation();
    bool reopen_item(int index);

    // close 管线本帧末的可观察进度快照：帧泵间完全不变 = 停滞（供逃生路径计数）。
    struct CloseProgress {
        int stack_size;
        uint32_t idle_baseline_pumps;
        bool close_pending;
        bool retry_close_top;
        bool pop_issued;
        bool differs_from(const CloseProgress& other) const {
            return stack_size != other.stack_size ||
                   idle_baseline_pumps != other.idle_baseline_pumps ||
                   close_pending != other.close_pending ||
                   retry_close_top != other.retry_close_top || pop_issued != other.pop_issued;
        }
    };
    // 每次帧泵末调用：close 管线在途且可观察进度连续 kCloseStallFrameLimit 帧无变化时
    // 强制收尾（force_complete_close）；有任一进度变化即清零重计。
    void track_close_stall();
    // 逃生收尾：补 close 内部栈残留 provider、pop 只补发一次（pop_issued_ 已发不重复）、
    // 尽力释放 popup entry、清空全部 close 状态机并记录超限原因与上限值。
    void force_complete_close();

    const char* popup_symbol_;
    PopupHostHooks hooks_;
    contracts::QolPopupStateEntry popup_entry_{};
    const contracts::PanelProviderSpec* pending_provider_ = nullptr;
    StackItem stack_[kStackCapacity]{};
    int stack_size_ = 0;
    uint32_t dispatch_depth_ = 0;
    uint64_t open_generation_counter_ = 0;
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
    bool retry_close_top_ = false;
    const char* pending_deactivate_owner_ = nullptr;
    bool pumping_ = false;
    uint32_t idle_baseline_pumps_ = 0;
    bool deactivating_ = false;
    bool main_thread_ = false;
    uint32_t process_dispatch_count_ = 0;
    int last_open_stack_size_ = -1;
    bool last_open_main_thread_ = false;
    bool last_open_deactivating_ = false;
    bool last_open_close_pending_ = false;
    bool last_snapshot_log_valid_ = false;
    bool last_snapshot_log_ok_ = false;
    uint32_t last_snapshot_log_count_ = 0;
    uintptr_t last_snapshot_log_top_vma_ = 0;
    uint32_t close_stall_frames_ = 0;
    CloseProgress close_stall_progress_{};
};

bool ui_panels_overlay_scope_matches(const void* provider, uint64_t generation);
bool ui_panels_custom_backend_active();
bool ui_panels_custom_overlay_scope_identity(const void** provider, uint64_t* generation);
// 无 PopupHost（未绑定/已解绑）时返回 false。
bool ui_panels_has_original_popup_baseline();
}  // namespace qol::features::core_ui
