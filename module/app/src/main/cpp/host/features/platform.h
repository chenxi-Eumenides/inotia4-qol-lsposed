#pragma once

#include "host/features/controller.h"

namespace qol::host {
constexpr const char* kPlatformConfigOwner = "module.platform";
struct FeaturePlatformState {
    uint64_t revision = 0;
    std::string listen_address = "0.0.0.0";
    int32_t listen_port = 8088;
    bool api_enabled = true;
    bool debug_logging = false;
    bool simple_mode = false;
};
using FeaturePlatformObserver = void (*)(void*, const FeaturePlatformState&);
const contracts::PluginDescriptor& platform_config_descriptor();
QolResult validate_platform_config(const char*, const char*, const JsonValue&);
QolResult feature_platform_subscribe(FeaturePlatformObserver, void*);
void feature_platform_publish(void*, const FeatureControlSnapshot&);
FeaturePlatformState feature_platform_state();
}  // namespace qol::host
