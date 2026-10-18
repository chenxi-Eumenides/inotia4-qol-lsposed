#include "contracts/move_merge.h"
#include "plugins/extension_bag/extension_bag_internal.h"
#include "plugins/extension_bag/move_merge.h"

bool move_merge_enabled() {
    if (g_sdk == nullptr || g_sdk->capabilities.require == nullptr) return false;
    const void* iface = nullptr;
    const ::QolResult result = g_sdk->capabilities.require(
        qol::contracts::kMoveMergeControlCapabilityId,
        {qol::contracts::kMoveMergeControlVersionMajor,
         qol::contracts::kMoveMergeControlVersionMinor},
        &iface);
    const auto* api = qol::contracts::succeeded(result)
                          ? static_cast<const qol::contracts::MoveMergeControlApi*>(iface)
                          : nullptr;
    return qol::contracts::move_merge_control_api_valid(api) && api->enabled != nullptr &&
           api->enabled();
}

bool set_extension_source_protection_enabled(bool enabled) {
    g_extension_source_protection_requested.store(enabled, std::memory_order_release);
    return true;
}
