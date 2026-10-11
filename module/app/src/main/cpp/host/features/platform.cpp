#include "platform.h"
#include "host/http_dispatch.h"
#include <algorithm>
#include <charconv>
#include <cstring>
#include <stdexcept>

namespace qol::host {
namespace {
std::mutex mutex;
FeaturePlatformState latest;
std::vector<std::pair<FeaturePlatformObserver, void*>> observers;
QolResult noop(contracts::PluginContext&, uint8_t) { return contracts::ok(); }
constexpr contracts::FeatureDefinition definitions[] = {
    {"api.enabled", 1, contracts::FeatureCategory::kGeneral, contracts::FeatureType::kToggle,
     "API服务", "控制 Host HTTP 服务及操作 API。", &noop, nullptr},
    {"debug.logging", 0, contracts::FeatureCategory::kGeneral, contracts::FeatureType::kToggle,
     "调试日志", "启用 Host 调试日志。", &noop, nullptr},
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
        value.feature_count = 2;
        return value;
    }();
    return descriptor;
}
QolResult validate_platform_config(const char* owner, const char* key, const JsonValue& value) {
    if (!owner || std::strcmp(owner, kPlatformConfigOwner) != 0) return contracts::ok();
    if (!key) return contracts::error(kQolErrInvalidArgument, nullptr, 0);
    if (std::strcmp(key, "listen_address") == 0) {
        if (value.kind != JsonValue::Kind::kString || value.scalar.empty() ||
            value.scalar.size() > 255 || value.scalar.find('\0') != std::string::npos)
            return contracts::error(kQolErrInvalidArgument, "invalid listen_address", 22);
    } else if (std::strcmp(key, "listen_port") == 0) {
        int32_t port = 0;
        const auto parsed = std::from_chars(value.scalar.data(), value.scalar.data() + value.scalar.size(), port);
        if (value.kind != JsonValue::Kind::kNumber || parsed.ec != std::errc{} ||
            parsed.ptr != value.scalar.data() + value.scalar.size() || port < 1 || port > 65535)
            return contracts::error(kQolErrInvalidArgument, "invalid listen_port", 19);
    }
    return contracts::ok();
}
QolResult feature_platform_subscribe(FeaturePlatformObserver observer, void* context) {
    if (!observer) return contracts::error(kQolErrInvalidArgument, nullptr, 0);
    FeaturePlatformState snapshot;
    {
        std::lock_guard<std::mutex> lock(mutex);
        const auto found = std::find(observers.begin(), observers.end(), std::make_pair(observer, context));
        if (found == observers.end()) observers.emplace_back(observer, context);
        snapshot = latest;
    }
    if (snapshot.revision) observer(context, snapshot);
    return contracts::ok();
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
            state.api_enabled = boolean(features, "api.enabled", true);
            // The API service is the sole runtime gate for op_gated routes. Legacy
            // features.op.enabled values remain inert in persisted config.
            state.debug_logging = boolean(features, "debug.logging", false);
            const auto* config = member(owner.second, "config");
            if (config) for (const auto& item : config->object) {
                const auto valid = validate_platform_config(kPlatformConfigOwner, item.first.c_str(), item.second);
                if (!contracts::succeeded(valid)) throw std::invalid_argument("invalid persisted platform configuration");
                if (item.first == "listen_address") state.listen_address = item.second.scalar;
                if (item.first == "listen_port") std::from_chars(item.second.scalar.data(), item.second.scalar.data() + item.second.scalar.size(), state.listen_port);
            }
        }
        if (owner.first == "simple_mode") {
            state.simple_mode = boolean(features, "simple_mode.combat", false);
            for (const auto& desired : snapshot.desired) if (desired.plugin_id == owner.first)
                state.simple_mode = state.simple_mode && desired.desired_enable;
        }
    }
    std::vector<std::pair<FeaturePlatformObserver, void*>> callbacks;
    {
        std::lock_guard<std::mutex> lock(mutex);
        if (state.revision <= latest.revision) return;
        latest = state;
        callbacks = observers;
    }
    http_dispatch_set_api_enabled(state.api_enabled);
    for (const auto& callback : callbacks) callback.first(callback.second, state);
}
}  // namespace qol::host
