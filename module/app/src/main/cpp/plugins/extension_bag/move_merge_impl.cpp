#include "contracts/move_merge.h"
#include "plugins/extension_bag/extension_bag_bridge.h"
#include "plugins/extension_bag/extension_bag_internal.h"
#include "plugins/extension_bag/extension_bag_port.h"
#include "plugins/extension_bag/game_ui_virtbag.h"
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

namespace qol::features::extension_bag {

bool merge_projection_available() {
    return inventory_extension_provider_active();
}

qol::contracts::MoveMergeProjectionDropResult merge_projection_drop(
    void* destination_control, void* source_control) {
    if (!merge_projection_available()) {
        return qol::contracts::MoveMergeProjectionDropResult::kNotHandled;
    }
    return static_cast<qol::contracts::MoveMergeProjectionDropResult>(
        virtual_bag_projection_drop_to_slot(destination_control, source_control));
}

QolResult merge_projection_identify_extension_item(qol::contracts::QolItemHandle item,
                                                    int32_t* out_bag,
                                                    int32_t* out_slot) {
    if (out_bag == nullptr || out_slot == nullptr || item.slot == 0 || item.generation == 0) {
        return qol::contracts::error(kQolErrInvalidArgument, "invalid item identity", 21);
    }
    if (!merge_projection_available()) {
        return qol::contracts::error(kQolErrNotReady, "merge projection unavailable", 29);
    }
    void* native_item = nullptr;
    if (!extension_bag_private_item_for_handle(item, &native_item) || native_item == nullptr) {
        return qol::contracts::error(kQolErrNotFound, "extension item not found", 24);
    }
    int32_t bag = -1;
    int32_t slot = -1;
    if (!virtual_bag_identify_native_item(native_item, &bag, &slot)) {
        return qol::contracts::error(kQolErrNotFound, "extension item not found", 24);
    }
    *out_bag = bag;
    *out_slot = slot;
    return qol::contracts::ok();
}

}  // namespace qol::features::extension_bag
