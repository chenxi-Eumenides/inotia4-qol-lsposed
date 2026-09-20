#include "core/native/extension_bag_port.h"

#include "contracts/item_count.h"
#include "host/adapters.h"

namespace {

const qol::contracts::ExtensionBagApi* extension_bag_api() {
    const auto& sdk = qol::host::g_host_sdk;
    if (sdk.capabilities.require == nullptr) return nullptr;
    const void* iface = nullptr;
    const QolResult result = sdk.capabilities.require(
        qol::contracts::kExtensionBagCapabilityId,
        QolVersionRange{static_cast<uint16_t>(qol::contracts::kExtensionBagVersionMajor),
                        static_cast<uint16_t>(qol::contracts::kExtensionBagVersionMinor)},
        &iface);
    if (result.ok != kQolOk) return nullptr;
    return static_cast<const qol::contracts::ExtensionBagApi*>(iface);
}

}  // namespace

const qol::contracts::ExtensionBagApi* extension_bag_capability() {
    return extension_bag_api();
}

void extension_bag_sync_projected_slot(int bag, int slot) {
    const auto* api = extension_bag_api();
    if (api != nullptr && api->sync_projected_slot != nullptr) api->sync_projected_slot(bag, slot);
}

void extension_bag_sync_projected_bag() {
    const auto* api = extension_bag_api();
    if (api != nullptr && api->sync_projected_bag != nullptr) api->sync_projected_bag();
}

bool extension_bag_module_view_installed() {
    const auto* api = extension_bag_api();
    return api != nullptr && api->module_view_installed != nullptr && api->module_view_installed();
}

bool extension_bag_enabled() {
    const auto* api = extension_bag_api();
    return api != nullptr && api->enabled != nullptr && api->enabled();
}

bool extension_bag_is_inventory_enter(uintptr_t enter) {
    const auto* api = extension_bag_api();
    return api != nullptr && api->is_inventory_enter != nullptr && api->is_inventory_enter(enter);
}

bool extension_bag_is_logical_bag(int bag) {
    const auto* api = extension_bag_api();
    return api != nullptr && api->is_logical_bag != nullptr && api->is_logical_bag(bag);
}

int extension_bag_internal_index(int logical_bag) {
    const auto* api = extension_bag_api();
    return api != nullptr && api->internal_index != nullptr ? api->internal_index(logical_bag) : -1;
}

bool extension_bag_prepare_main_menu() {
    const auto* api = extension_bag_api();
    return api != nullptr && api->prepare_main_menu != nullptr && api->prepare_main_menu();
}

int extension_bag_inventory_state_id() {
    const auto* api = extension_bag_api();
    return api != nullptr && api->inventory_state_id != nullptr ? api->inventory_state_id() : -1;
}

bool extension_bag_prepare_save_slot_load() {
    const auto* api = extension_bag_api();
    return api != nullptr && api->prepare_save_slot_load != nullptr && api->prepare_save_slot_load();
}

void extension_bag_for_each_logical_item(LogicalInventoryItemFn fn, void* ctx) {
    const auto* api = extension_bag_api();
    if (api != nullptr && api->for_each_logical_item != nullptr) {
        api->for_each_logical_item(fn, ctx);
    }
}

void* extension_bag_item_at(int bag, int slot) {
    const auto* api = extension_bag_api();
    return api != nullptr && api->item_at != nullptr ? api->item_at(bag, slot) : nullptr;
}

void* extension_bag_view_item_at(int bag, int slot) {
    const auto* api = extension_bag_api();
    return api != nullptr && api->view_item_at != nullptr ? api->view_item_at(bag, slot) : nullptr;
}

bool extension_bag_identify_native_item(void* item, int* out_bag, int* out_slot) {
    const auto* api = extension_bag_api();
    return api != nullptr && api->identify_native_item != nullptr &&
           api->identify_native_item(item, out_bag, out_slot);
}

void* extension_bag_find_native_item(int category) {
    const auto* api = extension_bag_api();
    return api != nullptr && api->find_native_item != nullptr ? api->find_native_item(category) : nullptr;
}

bool extension_bag_remove_native_item(void* item) {
    const auto* api = extension_bag_api();
    return api != nullptr && api->remove_native_item != nullptr && api->remove_native_item(item);
}

int64_t extension_bag_sell_price(void* item, int count, bool apply_variant_discount) {
    const auto* api = extension_bag_api();
    return api != nullptr && api->sell_price != nullptr
               ? api->sell_price(item, count, apply_variant_discount)
               : -1;
}

bool extension_bag_equip_projected_item(void* character, void* item, int source_bag,
                                        int source_slot, int equip_slot) {
    const auto* api = extension_bag_api();
    return api != nullptr && api->equip_projected_item != nullptr &&
           api->equip_projected_item(character, item, source_bag, source_slot, equip_slot);
}

void extension_bag_begin_internal_equip() {
    const auto* api = extension_bag_api();
    if (api != nullptr && api->begin_internal_equip != nullptr) api->begin_internal_equip();
}

void extension_bag_end_internal_equip() {
    const auto* api = extension_bag_api();
    if (api != nullptr && api->end_internal_equip != nullptr) api->end_internal_equip();
}

bool extension_bag_internal_equip_active() {
    const auto* api = extension_bag_api();
    return api != nullptr && api->internal_equip_active != nullptr && api->internal_equip_active();
}

bool extension_bag_consume_native_item(void* item) {
    const auto* api = extension_bag_api();
    return api != nullptr && api->consume_native_item != nullptr && api->consume_native_item(item);
}

int extension_bag_consume_category(int category, int count) {
    const auto* api = extension_bag_api();
    return api != nullptr && api->consume_category != nullptr ? api->consume_category(category, count) : 0;
}

bool extension_bag_has_empty_slots(int needed, int include_task_bag) {
    const auto* api = extension_bag_api();
    return api != nullptr && api->has_empty_slots != nullptr &&
           api->has_empty_slots(needed, include_task_bag);
}

bool extension_bag_adopt_unequipped_item(void* character, int equip_slot) {
    const auto* api = extension_bag_api();
    return api != nullptr && api->adopt_unequipped_item != nullptr &&
           api->adopt_unequipped_item(character, equip_slot);
}

bool extension_bag_adopt_native_item(void* item) {
    const auto* api = extension_bag_api();
    return api != nullptr && api->adopt_native_item != nullptr && api->adopt_native_item(item);
}

bool extension_bag_merge_native_item(void* item) {
    const auto* api = extension_bag_api();
    return api != nullptr && api->merge_native_item != nullptr && api->merge_native_item(item);
}

void extension_bag_store_restore_for_original_write() {
    const auto* api = extension_bag_api();
    if (api != nullptr && api->store_restore_for_original_write != nullptr) {
        api->store_restore_for_original_write();
    }
}

bool extension_bag_handle_backpack_button_equip() {
    const auto* api = extension_bag_api();
    return api != nullptr && api->handle_backpack_button_equip_result != nullptr &&
           api->handle_backpack_button_equip_result() ==
               qol::contracts::ExtensionBagEquipButtonResult::kHandled;
}

bool extension_bag_handle_original_bag_unequip(bool* out_no_space, bool* out_not_empty) {
    const auto* api = extension_bag_api();
    return api != nullptr && api->handle_original_bag_unequip != nullptr &&
           api->handle_original_bag_unequip(out_no_space, out_not_empty);
}
