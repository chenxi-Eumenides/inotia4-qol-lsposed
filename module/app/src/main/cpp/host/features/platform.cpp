#include "platform.h"
#include "infra/log/qol_log.h"

namespace qol::host {
namespace {
std::mutex mutex;
FeaturePlatformState latest;
QolResult noop(contracts::PluginContext&, uint8_t) { return contracts::ok(); }
constexpr contracts::FeatureDefinition definitions[] = {
    {"debug.logging", 0, contracts::FeatureCategory::kGeneral, contracts::FeatureType::kToggle,
     "debug日志", "启用 Host 调试日志。", &noop, nullptr, nullptr},
};
const JsonValue* member(const JsonValue& object, const char* key) {
    for (const auto& item : object.object) if (item.first == key) return &item.second;
    return nullptr;
}
bool boolean(const JsonValue* object, const char* key, bool fallback) {
    const auto* value = object ? member(*object, key) : nullptr;
    return value && value->kind == JsonValue::Kind::kBoolean ? value->boolean : fallback;
}
}
const contracts::PluginDescriptor& platform_config_descriptor() {
    static const contracts::PluginDescriptor descriptor = [] {
        contracts::PluginDescriptor value{};
        value.id = kPlatformConfigOwner;
        value.version = "1.0.0";
        value.features = definitions;
        value.feature_count = 1;
        return value;
    }();
    return descriptor;
}
const contracts::PluginDescriptor& security_config_descriptor() {
    static constexpr contracts::FeatureDefinition security_features[] = {
        {"op.enabled", 0, contracts::FeatureCategory::kGeneral,
         contracts::FeatureType::kToggle, "操作权限", "允许声明需要 OP 的操作。", &noop, nullptr,
         nullptr},
    };
    static const contracts::PluginDescriptor descriptor = [] {
        contracts::PluginDescriptor value{};
        value.id = kSecurityConfigOwner;
        value.version = "1.0.0";
        value.features = security_features;
        value.feature_count = 1;
        return value;
    }();
    return descriptor;
}
FeaturePlatformState feature_platform_state() {
    std::lock_guard<std::mutex> lock(mutex);
    return latest;
}
void feature_platform_publish(void*, const FeatureControlSnapshot& snapshot) {
    FeaturePlatformState state;
    state.revision = snapshot.revision;
    for (const auto& owner : snapshot.namespaces) {
        const auto* features = member(owner.second, "features");
        if (owner.first == kPlatformConfigOwner) {
            state.debug_logging = boolean(features, "debug.logging", false);
        }
    }
    {
        std::lock_guard<std::mutex> lock(mutex);
        if (state.revision <= latest.revision) return;
        latest = state;
    }
    qol_log_set_debug_enabled(state.debug_logging);
}
}  // namespace qol::host
