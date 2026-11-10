#include "reconciler.h"

#include "host/owner_identity.h"
#include "infra/capabilities.h"
#include "infra/log/qol_log.h"
#include <cstring>

namespace qol::host {
namespace {
QolResult error(int32_t code) { return contracts::error(code, nullptr, 0); }
class BusinessLease {
public:
    explicit BusinessLease(contracts::QolOwnerToken token)
        : result(owner_namespace_call_begin(token, &call)) {}
    ~BusinessLease() { owner_namespace_call_end(&call); }
    OwnerNamespaceCall call;
    QolResult result;
};
}  // namespace

FeatureReconciler::FeatureReconciler(FeatureController& controller) : controller_(controller) {
    controller_.bind_state_observer(&pending_state, this);
}

FeatureReconciler::~FeatureReconciler() {
    controller_.shutdown();
    controller_.bind_state_observer(nullptr, nullptr);
    if (bound_) {
        feature_registry_bind_desired_request(nullptr, nullptr);
        controller_.bind_entry_admission(nullptr, nullptr);
    }
}

void FeatureReconciler::pending_desired(void* context, uint64_t revision,
                                       const std::vector<PluginDesiredSnapshot>& desired) {
    auto& self = *static_cast<FeatureReconciler*>(context);
    std::lock_guard<std::mutex> lock(self.mutex_);
    if (revision <= self.pending_revision_) return;
    self.pending_ = desired;
    self.pending_revision_ = revision;
    self.toggles_dirty_ = true;
}

void FeatureReconciler::pending_state(void* context, const FeatureControlSnapshot& snapshot) {
    auto& self = *static_cast<FeatureReconciler*>(context);
    std::lock_guard<std::mutex> lock(self.mutex_);
    if (snapshot.revision < self.pending_revision_) return;
    self.pending_ = snapshot.desired;
    self.pending_namespaces_ = snapshot.namespaces;
    self.pending_revision_ = snapshot.revision;
    self.toggles_dirty_ = true;
}

bool FeatureReconciler::bind(const HostDeps& deps) {
    if (bound_) return false;
    uint32_t ordinary = 0;
    for (const auto& plugin : controller_.catalog()) {
        if (controller_.host_owned(plugin.plugin_id.c_str())) continue;
        ++ordinary;
        const auto* descriptor = find_plugin(plugin.plugin_id.c_str());
        if (!descriptor || !descriptor->features || descriptor->feature_count != plugin.features.size()) {
            QOL_LOG_ERROR(QolDomain::kCore, "feature bind: descriptor mismatch id=%s count=%u descriptor_count=%u",
                          plugin.plugin_id.c_str(), static_cast<unsigned>(plugin.features.size()),
                          descriptor != nullptr ? static_cast<unsigned>(descriptor->feature_count) : 0U);
            return false;
        }
        for (uint32_t f = 0; f < descriptor->feature_count; ++f) {
            if (std::strcmp(descriptor->features[f].id, plugin.features[f].id) != 0) {
                QOL_LOG_ERROR(QolDomain::kCore, "feature bind: feature id mismatch id=%s", plugin.plugin_id.c_str());
                return false;
            }
        }
    }
    if (ordinary != plugin_count()) {
        QOL_LOG_ERROR(QolDomain::kCore, "feature bind: plugin count mismatch ordinary=%u registered=%u",
                      static_cast<unsigned>(ordinary), static_cast<unsigned>(plugin_count()));
        return false;
    }
    if (!feature_registry_prepare(deps)) {
        QOL_LOG_ERROR(QolDomain::kCore, "feature bind: registry prepare failed");
        return false;
    }
    // bind() runs on the bridge-init thread (assembly), not the game thread; the main-thread
    // reference is captured lazily by the first step() so frame-driven work is only accepted
    // from the game's logical thread.
    bound_ = true;
    feature_registry_bind_desired_request(&request_desired, this);
    controller_.bind_entry_admission(&entry_admission, this);
    return true;
}

void FeatureReconciler::notify_dependency_changed() {
    std::lock_guard<std::mutex> lock(mutex_);
    dependency_dirty_ = true;
    toggles_dirty_ = true;
}
bool FeatureReconciler::request_desired(void* context, const char* id, bool enabled) {
    contracts::FeatureReceipt receipt{};
    return contracts::succeeded(static_cast<FeatureReconciler*>(context)->controller_
        .request_desired(id, enabled, &receipt));
}
QolResult FeatureReconciler::entry_admission(void* context, const char* plugin, const char* feature) {
    return static_cast<FeatureReconciler*>(context)->admit_entry(plugin, feature);
}
QolResult FeatureReconciler::admit_entry(const char* plugin, const char* feature) {
    if (!plugin || !feature) return error(kQolErrInvalidArgument);
    std::lock_guard<std::mutex> lock(mutex_);
    if (!bound_) return error(kQolErrNotReady);
    if (entries_.size() >= 256) return error(kQolErrBusy);
    entries_.emplace_back(plugin, feature);
    return contracts::ok();
}
bool FeatureReconciler::wanted(void* context, const char* id) {
    auto& self = *static_cast<FeatureReconciler*>(context);
    const auto found = self.eligible_.find(id);
    return found != self.eligible_.end() && found->second;
}

bool FeatureReconciler::business_ready(const char* plugin, const char* feature,
                                      contracts::PluginContext* out) {
    if (!bound_ || std::this_thread::get_id() != main_thread_) return false;
    const auto desired = desired_.find(plugin);
    if (desired == desired_.end() || !desired->second.want_run() || !wanted(this, plugin) ||
        !feature_registry_dependencies_ready(plugin) || !feature_registry_context(plugin, out)) return false;
    const auto value = feature_values_.find({plugin, feature});
    if (value == feature_values_.end() || !value->second) return false;
    const auto* descriptor = find_plugin(plugin);
    for (uint32_t f = 0; descriptor && f < descriptor->feature_count; ++f) {
        if (std::strcmp(descriptor->features[f].id, feature) == 0) return true;
    }
    return false;
}

QolResult FeatureReconciler::probe_availability(const char* plugin, const char* feature,
                                               AvailabilityProbe probe, void* context) {
    if (!plugin || !feature || !probe) return error(kQolErrInvalidArgument);
    contracts::PluginContext ctx{};
    if (!business_ready(plugin, feature, &ctx)) return error(kQolErrNotReady);
    BusinessLease lease(ctx.owner_token);
    if (!contracts::succeeded(lease.result)) return lease.result;
    try { return probe(ctx, context); }
    catch (...) { return error(kQolErrInternal); }
}

void FeatureReconciler::notify_toggles() {
    for (const auto& plugin : controller_.catalog()) {
        if (controller_.host_owned(plugin.plugin_id.c_str())) continue;
        contracts::PluginContext ctx{};
        if (!feature_registry_context(plugin.plugin_id.c_str(), &ctx) ||
            !feature_registry_dependencies_ready(plugin.plugin_id.c_str())) continue;
        const bool rebound = notified_tokens_[plugin.plugin_id] != ctx.owner_token;
        notified_tokens_[plugin.plugin_id] = ctx.owner_token;
        for (const auto& feature : plugin.features) {
            const auto key = std::make_pair(plugin.plugin_id, std::string(feature.id));
            if (feature.type == contracts::FeatureType::kToggle) {
                const auto value = feature_values_.find(key);
                if (value == feature_values_.end()) continue;
                const bool enabled = value->second;
                const auto last = notified_.find(key);
                if (!rebound && last != notified_.end() && last->second == (enabled != 0)) continue;
                BusinessLease lease(ctx.owner_token);
                if (!contracts::succeeded(lease.result)) break;
                // Mark before dispatch so a reentrant safe point does not redeliver this item.
                notified_[key] = enabled != 0;
                try { callback_error_ = feature.on_toggle(ctx, enabled ? 1 : 0).code; }
                catch (...) { callback_error_ = kQolErrInternal; }
            } else if (feature.type == contracts::FeatureType::kChoice) {
                const auto value = choice_values_.find(key);
                const std::string selected = value == choice_values_.end() ? std::string{} : value->second;
                const auto last = notified_choices_.find(key);
                if (!rebound && last != notified_choices_.end() && last->second == selected) continue;
                BusinessLease lease(ctx.owner_token);
                if (!contracts::succeeded(lease.result)) break;
                notified_choices_[key] = selected;
                try { callback_error_ = feature.on_choice(ctx, selected.empty() ? nullptr : selected.c_str()).code; }
                catch (...) { callback_error_ = kQolErrInternal; }
            }
        }
    }
}

uint32_t FeatureReconciler::step() {
    if (!bound_) return 0;
    if (main_thread_ == std::thread::id{}) main_thread_ = std::this_thread::get_id();
    if (std::this_thread::get_id() != main_thread_) return 0;
    // 重试预算按 step() 调用次数计时（无时间预算/线程）；主线程之外的调用不计。
    ++step_clock_;

    bool pending = false;
    bool toggles_dirty = false;
    bool pending_work = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const uint64_t capability_revision = infra::capability_revision();
        if (capability_revision != last_capability_revision_) {
            // 能力注册/撤销会改变依赖就绪边；O(1) 检测，避免每帧重扫插件图。
            last_capability_revision_ = capability_revision;
            dependency_dirty_ = true;
        }
        pending_work = feature_registry_has_pending_work();
        if (had_pending_work_ && !pending_work) {
            // 在途生命周期事务刚排空：下一帧重算一次依赖边，但不 bump retry batch，
            // 避免失败插件在每次事务排空后被无限重试。
            full_step_pending_ = true;
        }
        had_pending_work_ = pending_work;
        pending = pending_revision_ > revision_ || dependency_dirty_ || !entries_.empty();
        toggles_dirty = toggles_dirty_;
    }
    // 无配置/依赖/entry 变化、无在途生命周期事务时，正常帧不应重跑整张插件图。
    // 每 300 次空闲 step 强制全量一次，作为漏标记的兜底；首次 step 必须全量。
    const bool force_full = !initialized_ || idle_steps_ >= 300 || activation_retry_ > 0;
    if (!force_full && retry_.empty() && !pending && !full_step_pending_ && !toggles_dirty && !pending_work) {
        ++idle_steps_;
        return 0;
    }
    if (!force_full && retry_.empty() && !pending && !full_step_pending_ && !pending_work) {
        // 只有 feature callback 投递待处理（例如 revision 已应用但回调尚未送达）。
        notify_toggles();
        std::lock_guard<std::mutex> lock(mutex_);
        toggles_dirty_ = pending_revision_ > revision_;
        idle_steps_ = 0;
        return 0;
    }
    initialized_ = true;
    idle_steps_ = 0;
    full_step_pending_ = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (pending_revision_ > revision_) {
            desired_.clear();
            for (const auto& snapshot : pending_) desired_.emplace(snapshot.plugin_id, snapshot);
            feature_values_.clear();
            choice_values_.clear();
            for (const auto& plugin : pending_namespaces_) {
                for (const auto& section : plugin.second.object) {
                    if (section.first == "features") {
                        for (const auto& value : section.second.object) {
                            if (value.second.kind == JsonValue::Kind::kBoolean)
                                feature_values_[{plugin.first, value.first}] = value.second.boolean;
                        }
                    } else if (section.first == "selections") {
                        for (const auto& value : section.second.object) {
                            if (value.second.kind == JsonValue::Kind::kString)
                                choice_values_[{plugin.first, value.first}] = value.second.scalar;
                        }
                    }
                }
            }
            revision_ = pending_revision_;
            ++batch_;
            // 用户显式变更配置：重置瞬态失败的重试预算，按新配置重新判定。
            retry_.clear();
        }
        if (dependency_dirty_) { dependency_dirty_ = false; ++batch_; }
    }
    const char* order[32]{};
    const auto count = feature_registry_order(order, 32);
    // A dependency becoming available is a one-shot topology event equivalent to
    // notify_dependency_changed(): bump the batch so a dependent's failed/blocked
    // activation is retried exactly once, never per-frame. This is the production
    // recovery seam (registry capability re-register after provider stop/resume).
    bool dependency_edge = false;
    for (uint32_t i = 0; i < count; ++i) {
        const bool ready = feature_registry_dependencies_ready(order[i]);
        const auto inserted = dependency_ready_.emplace(order[i], ready);
        if (!inserted.second) {
            if (!inserted.first->second && ready) dependency_edge = true;
            inserted.first->second = ready;
        }
    }
    if (dependency_edge) {
        std::lock_guard<std::mutex> lock(mutex_);
        ++batch_;
    }
    eligible_.clear();
    for (uint32_t i = 0; i < count; ++i) {
        const auto desired = desired_.find(order[i]);
        const bool eligible = desired != desired_.end() && desired->second.want_run() &&
                              feature_registry_dependencies_wanted(order[i], &wanted, this);
        eligible_[order[i]] = eligible;
        feature_registry_set_wanted(order[i], eligible);
    }
    // Deliver changed toggle states while the old instance is still actually active,
    // including the last false toggle; the following stop then revokes its admission.
    notify_toggles();
    uint32_t advanced = feature_registry_step();
    // 安装事务在 step 内部以 NotReady 失败终态时，激活循环本帧已经去重；消费 registry
    // 带出的一次性标记，按有界预算安排在后续帧重试。已有预算不重置。
    for (uint32_t i = 0; i < count; ++i) {
        if (!feature_registry_pop_transient_failure(order[i])) continue;
        const auto inserted = retry_.try_emplace(order[i]);
        if (inserted.second) {
            inserted.first->second.remaining = 3;
            inserted.first->second.next_step = step_clock_ + 5;
        }
    }
    // A full reverse-topological stop barrier prevents provider cleanup before consumers
    // have finished owner/resource drain. Installs that become unwanted finish into teardown.
    for (uint32_t i = count; i > 0; --i) {
        const auto* id = order[i - 1];
        if (feature_registry_busy(id) && !wanted(this, id)) return advanced;
        if (feature_registry_active(id) &&
            (!wanted(this, id) || !feature_registry_dependencies_ready(id))) {
            // Permanent policy rejection: keep the plugin active instead of retrying every step.
            const auto* descriptor = find_plugin(id);
            if (descriptor != nullptr && !descriptor->deactivate_allowed) continue;
            if (feature_registry_transition(id, false)) {
                retry_.erase(id);
                ++advanced;
            }
            return advanced;
        }
    }
    // 到期重试：清除同批去重标记，让本次激活循环重新尝试该插件。remaining 是总预算，
    // next_step 只决定下一次尝试的 step 序号（间隔 5 步，步进只算 step() 调用）。
    for (auto it = retry_.begin(); it != retry_.end();) {
        if (!wanted(this, it->first.c_str())) {
            it = retry_.erase(it);
            continue;
        }
        if (step_clock_ < it->second.next_step) {
            ++it;
            continue;
        }
        // 预算耗尽：只清表项，不解除同批去重，避免本帧又产生一次额外尝试。
        if (it->second.remaining == 0) {
            it = retry_.erase(it);
            continue;
        }
        attempted_.erase(it->first);
        --it->second.remaining;
        it->second.next_step = step_clock_ + 5;
        ++it;
    }
    for (uint32_t i = 0; i < count; ++i) {
        const auto* id = order[i];
        if (!wanted(this, id) || feature_registry_active(id) || feature_registry_busy(id) ||
            !feature_registry_dependencies_ready(id) || attempted_[id] == batch_) continue;
        if (!feature_registry_owner_ready(id)) {
            activation_retry_ = 8;
            continue;
        }
        attempted_[id] = batch_;
        if (feature_registry_transition(id, true)) {
            retry_.erase(id);
            ++advanced;
        } else if (feature_registry_last_failure_code(id) == kQolErrNotReady) {
            // 首次瞬态失败：最多再试 3 次；已有预算不因重复失败而重置。
            const auto inserted = retry_.try_emplace(id);
            if (inserted.second) {
                inserted.first->second.remaining = 3;
                inserted.first->second.next_step = step_clock_ + 5;
            }
        } else {
            retry_.erase(id);
        }
    }
    notify_toggles();
    std::deque<std::pair<std::string, std::string>> entries;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        entries.swap(entries_);
    }
    for (const auto& request : entries) {
        contracts::PluginContext ctx{};
        if (!business_ready(request.first.c_str(), request.second.c_str(), &ctx)) continue;
        const auto* plugin = find_plugin(request.first.c_str());
        if (!plugin) continue;
        for (uint32_t f = 0; f < plugin->feature_count; ++f) {
            const auto& feature = plugin->features[f];
            if (request.second != feature.id || feature.type != contracts::FeatureType::kEntry) continue;
            BusinessLease lease(ctx.owner_token);
            if (!contracts::succeeded(lease.result)) break;
            try { callback_error_ = feature.on_entry(ctx).code; }
            catch (...) { callback_error_ = kQolErrInternal; }
            break;
        }
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        // 本轮已投递；若 step 期间又来了新 revision，保留 dirty 供下一帧处理。
        toggles_dirty_ = pending_revision_ > revision_;
        if (activation_retry_ > 0) --activation_retry_;
    }
    return advanced;
}
}  // namespace qol::host
