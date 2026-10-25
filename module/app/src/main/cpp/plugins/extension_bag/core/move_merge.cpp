#include "plugins/extension_bag/core/move_merge.h"

#include <atomic>

#include "contracts/move_merge.h"
#include "plugins/extension_bag/core/ui_state.h"
#include "plugins/extension_bag/runtime/state.h"

bool move_merge_enabled() {
    if (runtime_state().host.sdk == nullptr ||
        runtime_state().host.sdk->capabilities.require == nullptr) {
        return false;
    }
    const void* iface = nullptr;
    const ::QolResult result = runtime_state().host.sdk->capabilities.require(
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
    qol::extension_bag::ui_state().aux.source_protection_requested.store(
        enabled, std::memory_order_release);
    return true;
}
