#include "contracts/inventory_action.h"
#include "contracts/inventory_refresh.h"
#include "contracts/inventory_slot.h"
#include "plugins/core.item/inventory/inventory_slot.h"
#include "plugins/core.item/inventory/plugin.h"
#include "plugins/core.item/merge/plugin.h"

#include <array>
#include <cstring>

namespace qol::features::inventory_hooks {
namespace {

contracts::InventorySellResult sell_unavailable(int32_t, int32_t,
                                                contracts::QolItemHandle, const char*) {
    return {contracts::InventorySellStatus::kSymbolsUnresolved, 0, 0, 0};
}

bool refresh_unavailable() { return false; }

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

const contracts::InventoryRefreshApi kInventoryRefreshApi{&refresh_unavailable};
const contracts::InventoryActionApi kInventoryActionApi{&sell_unavailable};
const contracts::QolInventorySlotApi kInventorySlotApi{
    sizeof(contracts::QolInventorySlotApi), &slot_unavailable, &slot_view_unavailable,
    &slot_remove_unavailable};

bool attach(const contracts::QolHostSdk*, const contracts::QolGameHost*,
            const contracts::ItemFactsApi*) {
    return true;
}

void detach() {}

const contracts::ResourceClaim* claims(uint32_t* out_count) {
    static std::array<contracts::ResourceClaim, 24> resources = [] {
        std::array<contracts::ResourceClaim, 24> result{};
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

namespace qol::features::core_item::merge {
namespace {

bool g_wrapper_ready = false;
bool g_feature_enabled = true;

bool control_enabled() { return g_feature_enabled && g_wrapper_ready; }

}  // namespace

const contracts::MoveMergeControlApi kMoveMergeControlApi{
    sizeof(contracts::MoveMergeControlApi), contracts::kMoveMergeControlVersionMajor,
    contracts::kMoveMergeControlVersionMinor, &control_enabled};

const contracts::ResourceClaim* claims(uint32_t* out_count) {
    static const contracts::ResourceClaim resource{
        "ptrslot:MOVE_MERGE_GOT", "ptrslot:MOVE_MERGE_GOT",
        contracts::ResourceMechanism::kPtrSlot, contracts::ResourcePolicy::kPrivate,
        contracts::ResourceRequirement::kOptional, contracts::ResourceFailure::kFailClosed,
        true, "core.item", "test fixture", nullptr, 120, reinterpret_cast<void*>(1)};
    if (out_count != nullptr) *out_count = 1;
    return &resource;
}

bool attach(const contracts::QolHostSdk*, const contracts::QolMoveMergePort*,
            const contracts::QolCharacterExtrasPort*, const contracts::ItemFactsApi*) {
    return true;
}

void detach() {
    g_wrapper_ready = false;
    g_feature_enabled = false;
}
void set_enabled(bool enabled) { g_feature_enabled = enabled; }
bool enabled() { return g_feature_enabled; }
bool wrapper_ready() { return g_wrapper_ready; }
void set_wrapper_ready(bool ready) { g_wrapper_ready = ready; }
bool is_merge_resource(const char* resource_id) {
    return resource_id != nullptr &&
           std::strcmp(resource_id, "ptrslot:MOVE_MERGE_GOT") == 0;
}

}  // namespace qol::features::core_item::merge
