#include "plugins/extension_bag/assembly/projection_registration.h"

#include "contracts/inventory_projection.h"
#include "contracts/move_merge.h"
#include "plugins/extension_bag/game_ui_virtbag.h"
#include "plugins/extension_bag/core/host_adapter.h"
#include "plugins/extension_bag/ui/host_ui.h"
#include "plugins/extension_bag/runtime/extension_bag_port.h"
#include "plugins/extension_bag/runtime/reentry_lock.h"
#include "plugins/extension_bag/runtime/state.h"

namespace qol::features::extension_bag {
namespace {

bool projection_available() { return extension_bag_enabled(); }

qol::contracts::MoveMergeProjectionDropResult projection_drop(void* destination_control,
                                                              void* source_control) {
    if (!projection_available()) {
        return qol::contracts::MoveMergeProjectionDropResult::kNotHandled;
    }
    return static_cast<qol::contracts::MoveMergeProjectionDropResult>(
        virtual_bag_projection_drop_to_slot(destination_control, source_control));
}

bool projection_original_item_input_blocked() {
    return projection_available() && virtual_bag_original_item_input_blocked();
}

bool projection_allow_original_tab_drop(void* control, void* source_control,
                                        uint64_t caller_offset) {
    return projection_available() &&
           virtual_bag_allow_original_tab_drop(control, source_control, caller_offset);
}

QolResult projection_identify_extension_item(qol::contracts::QolItemHandle item,
                                             int32_t* out_bag, int32_t* out_slot) {
    if (out_bag == nullptr || out_slot == nullptr || item.slot == 0 || item.generation == 0) {
        return qol::contracts::error(kQolErrInvalidArgument, "invalid item identity", 21);
    }
    if (!projection_available()) {
        return qol::contracts::error(kQolErrNotReady, "projection unavailable", 29);
    }
    void* native_item = nullptr;
    if (!extension_bag_private_item_for_handle(item, &native_item) || native_item == nullptr) {
        return qol::contracts::error(kQolErrNotFound, "extension item not found", 24);
    }
    ::qol::extension_bag::ReentryLock lock;
    int32_t bag = -1;
    int32_t slot = -1;
    if (!module_slot_of_item_locked(native_item, &bag, &slot)) {
        return qol::contracts::error(kQolErrNotFound, "extension item not found", 24);
    }
    *out_bag = bag;
    *out_slot = slot;
    return qol::contracts::ok();
}

uint64_t projection_observe_pre(void* control, uint64_t event, void* x2, void* param) {
    return projection_available()
               ? virtual_bag_observe_item_proc_pre(control, event, x2, param)
               : 0;
}

void projection_observe_source(uint64_t observation_token, void* control, uint64_t event,
                               void* x2, void* param, void* source_control) {
    if (projection_available()) {
        virtual_bag_observe_item_proc_source(observation_token, control, event, x2, param,
                                             source_control);
    }
}

void projection_observe_post(uint64_t observation_token, void* control, uint64_t event,
                             void* x2, void* param, uint64_t result) {
    if (projection_available()) {
        virtual_bag_observe_item_proc_post(observation_token, control, event, x2, param, result);
    }
}

bool projection_module_view_installed() {
    return projection_available() && virtual_bag_module_view_installed();
}

bool projection_sync_slot(int32_t display_bag, int32_t slot) {
    return projection_available() && virtual_bag_sync_projected_slot(display_bag, slot);
}

bool projection_sync_bag() {
    return projection_available() && virtual_bag_sync_projected_bag();
}

bool projection_handle_original_bag_unequip() {
    return projection_available() && extension_handle_original_bag_unequip();
}

bool projection_handle_use_mercenary_seal() {
    return projection_available() && extension_use_mercenary_seal();
}

bool projection_handle_backpack_equip() {
    return projection_available() && extension_handle_backpack_equip();
}

bool projection_handle_character_equip() {
    return projection_available() && extension_handle_character_equip();
}

bool projection_handle_extension_sell() {
    return projection_available() && extension_handle_sell_button();
}

const qol::contracts::InventoryProjectionCallbacks kProjectionCallbacks{
    sizeof(qol::contracts::InventoryProjectionCallbacks),
    qol::contracts::kInventoryProjectionVersionMajor,
    qol::contracts::kInventoryProjectionVersionMinor,
    &projection_drop,
    &projection_original_item_input_blocked,
    &projection_allow_original_tab_drop,
    &projection_identify_extension_item,
    &projection_observe_pre,
    &projection_observe_source,
    &projection_observe_post,
    &projection_module_view_installed,
    &projection_sync_slot,
    &projection_sync_bag,
    &projection_handle_original_bag_unequip,
    &projection_handle_use_mercenary_seal,
    &projection_handle_backpack_equip,
    &projection_handle_character_equip,
    &projection_handle_extension_sell,
};

const qol::contracts::InventoryProjectionRegistryApi* projection_registry() {
    const auto* sdk = runtime_state().host.sdk;
    if (sdk == nullptr || sdk->capabilities.require == nullptr) return nullptr;
    const void* iface = nullptr;
    const auto result = sdk->capabilities.require(
        qol::contracts::kInventoryProjectionCapabilityId,
        {qol::contracts::kInventoryProjectionVersionMajor,
         qol::contracts::kInventoryProjectionVersionMinor},
        &iface);
    const auto* api = qol::contracts::succeeded(result)
                          ? static_cast<const qol::contracts::InventoryProjectionRegistryApi*>(iface)
                          : nullptr;
    return qol::contracts::inventory_projection_registry_api_valid(api) ? api : nullptr;
}

}  // namespace

QolResult register_projection_callbacks() {
    const auto* registry = projection_registry();
    if (registry == nullptr) {
        return qol::contracts::error(kQolErrNotReady, "inventory.projection unavailable", 40);
    }
    return registry->register_callbacks(&kProjectionCallbacks);
}

void unregister_projection_callbacks() {
    const auto* registry = projection_registry();
    if (registry != nullptr) {
        (void)registry->unregister_callbacks();
    }
}

}  // namespace qol::features::extension_bag
