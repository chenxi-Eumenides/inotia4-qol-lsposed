#pragma once

#include "host/features/controller.h"
#include "host/plugin_registry.h"

namespace qol::host {

// Loader-lifetime new-model adapter. The worker only delivers immutable desired data;
// step() is called at a logical frame safe point on the bound main thread.
class FeatureReconciler {
public:
    explicit FeatureReconciler(FeatureController& controller);
    ~FeatureReconciler();
    FeatureReconciler(const FeatureReconciler&) = delete;
    FeatureReconciler& operator=(const FeatureReconciler&) = delete;
    static void pending_desired(void*, uint64_t, const std::vector<PluginDesiredSnapshot>&);
    static void pending_state(void*, const FeatureControlSnapshot&);
    bool bind(const HostDeps&);
    void notify_dependency_changed();
    uint32_t step();
    uint64_t applied_revision() const { return revision_; }
    int32_t last_callback_error() const { return callback_error_; }
    QolResult admit_entry(const char* plugin, const char* feature);
    using AvailabilityProbe = QolResult (*)(contracts::PluginContext&, void*);
    QolResult probe_availability(const char*, const char*, AvailabilityProbe, void*);

private:
    static bool request_desired(void*, const char*, bool);
    static QolResult entry_admission(void*, const char*, const char*);
    static bool wanted(void*, const char*);
    bool business_ready(const char*, const char*, contracts::PluginContext*);
    void notify_toggles();
    FeatureController& controller_;
    std::mutex mutex_;
    std::vector<PluginDesiredSnapshot> pending_;
    std::vector<std::pair<std::string, JsonValue>> pending_namespaces_;
    uint64_t pending_revision_ = 0;
    bool dependency_dirty_ = false;
    // 配置/依赖变化后是否需要重新投递 feature callback；仅由 controller 回调和 step() 修改。
    bool toggles_dirty_ = true;
    // 连续空闲 step 计数；达到上限时强制跑一次全量 step，作为漏标记的兜底。
    uint32_t idle_steps_ = 0;
    // 首次 step 必须走全量路径：初始 desired/依赖边和默认启用插件都靠这一轮建立。
    bool initialized_ = false;
    // 上一帧是否存在在途生命周期事务；用于在事务排空后触发一次依赖边重算。
    bool had_pending_work_ = false;
    // 事务排空后需要一次全量 step，但不 bump retry batch（避免失败插件反复重试）。
    bool full_step_pending_ = false;
    // owner token 尚未就绪时的有界重试计数（仅激活循环使用）。
    uint32_t activation_retry_ = 0;
    // 瞬态（NotReady）激活失败的有界重试预算：next_step 为下一次允许尝试的 step 序号，
    // remaining 为剩余尝试次数。step 序号只按 step() 调用递增，不依赖时间。
    struct RetryState {
        uint8_t remaining = 0;
        uint32_t next_step = 0;
    };
    std::map<std::string, RetryState> retry_;
    uint32_t step_clock_ = 0;
    std::deque<std::pair<std::string, std::string>> entries_;
    std::map<std::string, PluginDesiredSnapshot> desired_;
    std::map<std::string, bool> eligible_;
    std::map<std::pair<std::string, std::string>, bool> feature_values_;
    std::map<std::string, uint64_t> attempted_;
    // Previous frame's per-plugin dependency readiness; a false→true edge is a one-shot
    // dependency event equivalent to notify_dependency_changed().
    std::map<std::string, bool> dependency_ready_;
    std::map<std::pair<std::string, std::string>, bool> notified_;
    std::map<std::pair<std::string, std::string>, std::string> notified_choices_;
    std::map<std::pair<std::string, std::string>, std::string> choice_values_;
    std::map<std::string, contracts::QolOwnerToken> notified_tokens_;
    std::thread::id main_thread_;
    bool bound_ = false;
    uint64_t revision_ = 0;
    uint64_t batch_ = 0;
    uint64_t last_capability_revision_ = 0;
    int32_t callback_error_ = 0;
};
}  // namespace qol::host
