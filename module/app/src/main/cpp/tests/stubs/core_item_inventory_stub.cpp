#include "contracts/inventory_action.h"
#include "contracts/inventory_slot.h"
#include "plugins/core.item/usecases/item_action.h"
#include "plugins/core.item/usecases/item_slot.h"
#include "plugins/core.item/usecases/sell_item.h"

#include <array>
#include <cstring>

namespace qol::features::inventory_hooks {
namespace {

contracts::InventorySellResult sell_unavailable(contracts::QolActionRef,
                                                contracts::QolItemHandle, int32_t,
                                                const char*) {
    return {contracts::InventorySellStatus::kSymbolsUnresolved, 0, 0, 0};
}

contracts::InventoryMoveOutcome move_unavailable(contracts::QolActionRef, int32_t,
                                                  contracts::QolActionRef target) {
    return {contracts::InventoryActionStatus::kUnsupported, 0, 0, target, {}};
}

contracts::InventoryUseOutcome use_unavailable(contracts::QolActionRef) {
    return {contracts::InventoryActionStatus::kUnsupported, 0};
}

contracts::InventoryUseOutcome discard_unavailable(contracts::QolActionRef, int32_t) {
    return {contracts::InventoryActionStatus::kUnsupported, 0};
}

contracts::InventoryUseOutcome equip_unavailable(int32_t, contracts::QolActionRef, int32_t) {
    return {contracts::InventoryActionStatus::kUnsupported, 0};
}

contracts::InventoryUseOutcome unequip_unavailable(int32_t, int32_t) {
    return {contracts::InventoryActionStatus::kUnsupported, 0};
}

contracts::InventoryUseOutcome put_jewel_unavailable(int32_t, contracts::QolActionRef,
                                                     int32_t) {
    return {contracts::InventoryActionStatus::kUnsupported, 0};
}

contracts::QolItemCreateOutcome create_unavailable(const contracts::QolItemCreateSpec*) {
    return {sizeof(contracts::QolItemCreateOutcome),
            contracts::InventoryActionStatus::kUnsupported, contracts::kInvalidItemHandle};
}

contracts::QolItemPlaceOutcome place_unavailable(contracts::QolItemHandle,
                                                 const contracts::QolItemPlaceTarget*) {
    return {sizeof(contracts::QolItemPlaceOutcome),
            contracts::InventoryActionStatus::kUnsupported, {}, contracts::kInvalidItemHandle,
            0, 0};
}

contracts::InventoryUseOutcome discard_handle_unavailable(contracts::QolItemHandle, int32_t) {
    return {contracts::InventoryActionStatus::kUnsupported, 0};
}

QolResult slot_unavailable(int32_t, int32_t, contracts::QolInventorySlotView*) {
    return contracts::error(kQolErrNotReady, "stub unavailable", 16);
}

QolResult slot_view_unavailable(contracts::QolItemHandle, contracts::QolInventorySlotView*) {
    return contracts::error(kQolErrNotReady, "stub unavailable", 16);
}

QolResult slot_remove_unavailable(int32_t, int32_t, contracts::QolItemHandle, uint32_t) {
    return contracts::error(kQolErrNotReady, "stub unavailable", 16);
}

}  // namespace

// inventory_refresh provider 已迁往 extension_bag；本桩只保留 core.item 自身能力。
const contracts::QolInventorySlotApi kInventorySlotApi{
    sizeof(contracts::QolInventorySlotApi), &slot_unavailable, &slot_view_unavailable,
    &slot_remove_unavailable};

bool attach(const contracts::QolHostSdk*, const contracts::QolGameHost*,
            const contracts::ItemFactsApi*) {
    return true;
}

void detach() {}

const contracts::ResourceClaim* claims(uint32_t* out_count) {
    static std::array<contracts::ResourceClaim, 11> resources = [] {
        std::array<contracts::ResourceClaim, 11> result{};
        for (uint32_t index = 0; index < result.size(); ++index) {
            result[index] = {
                "hook:stub", "stub", contracts::ResourceMechanism::kEntryHook,
                contracts::ResourcePolicy::kPrivate, contracts::ResourceRequirement::kRequired,
                contracts::ResourceFailure::kFailClosed, true, "core.item", "test fixture",
                nullptr, 120, reinterpret_cast<void*>(1)};
        }
        return result;
    }();
    if (out_count != nullptr) *out_count = static_cast<uint32_t>(resources.size());
    return resources.data();
}

int32_t bind_backup(const char*, void*) { return 0; }

}  // namespace qol::features::inventory_hooks

namespace qol::features::core_item {

const contracts::InventoryActionApi kInventoryActionApi{
    sizeof(contracts::InventoryActionApi), contracts::kInventoryActionVersionMajor,
    contracts::kInventoryActionVersionMinor, &qol::features::inventory_hooks::move_unavailable,
    &qol::features::inventory_hooks::use_unavailable,
    &qol::features::inventory_hooks::discard_unavailable,
    &qol::features::inventory_hooks::sell_unavailable,
    &qol::features::inventory_hooks::equip_unavailable,
    &qol::features::inventory_hooks::unequip_unavailable,
    &qol::features::inventory_hooks::put_jewel_unavailable,
    &qol::features::inventory_hooks::create_unavailable,
    &qol::features::inventory_hooks::place_unavailable,
    &qol::features::inventory_hooks::discard_handle_unavailable};

}  // namespace qol::features::core_item


