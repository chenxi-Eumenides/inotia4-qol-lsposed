#include "contracts/plugin.h"
#include "plugins/popup_blocker/plugin.h"

#include <cassert>
#include <cstdint>
#include <cstring>

int main() {
    const qol::contracts::PluginDescriptor& descriptor =
        qol::features::popup_blocker::plugin_descriptor();
    assert(std::strcmp(descriptor.id, "popup_blocker") == 0);
    assert(descriptor.resources_count == 2);
    assert(descriptor.resources != nullptr);

    const qol::contracts::ResourceClaim& pause_gate = descriptor.resources[0];
    assert(std::strcmp(pause_gate.id, "P_TUTORIAL_PAUSE_GATE_SKIP") == 0);
    assert(pause_gate.mechanism == qol::contracts::ResourceMechanism::kCallPatch);
    assert(pause_gate.atomic);

    const qol::contracts::ResourceClaim& tutorial_description = descriptor.resources[1];
    assert(std::strcmp(tutorial_description.id, "hook:TutorialDescription") == 0);
    assert(tutorial_description.mechanism == qol::contracts::ResourceMechanism::kEntryHook);
    assert(tutorial_description.handler != nullptr);
    assert(descriptor.on_resource_ready == nullptr);
    using TutorialDescriptionHook = int32_t (*)();
    const auto handler = reinterpret_cast<TutorialDescriptionHook>(tutorial_description.handler);
    assert(handler() == 0);

    assert(descriptor.feature_count == 1);
    assert(std::strcmp(descriptor.features[0].id, "popup_blocker.enabled") == 0);
    assert(descriptor.features[0].default_enabled == 1);
    assert(descriptor.features[0].category ==
           qol::contracts::FeatureCategory::kBasicOptimization);
    assert(std::strcmp(descriptor.features[0].name, "禁用弹窗") == 0);
    return 0;
}
