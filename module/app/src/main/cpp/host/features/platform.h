#pragma once

#include "host/features/controller.h"

namespace qol::host {
constexpr const char* kPlatformConfigOwner = "module.platform";
constexpr const char* kSecurityConfigOwner = "module.security";
struct FeaturePlatformState {
    uint64_t revision = 0;
    bool debug_logging = false;
};
const contracts::PluginDescriptor& platform_config_descriptor();
const contracts::PluginDescriptor& security_config_descriptor();
void feature_platform_publish(void*, const FeatureControlSnapshot&);
FeaturePlatformState feature_platform_state();
}  // namespace qol::host
