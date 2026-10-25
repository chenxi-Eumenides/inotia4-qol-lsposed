#include "plugin.h"

#include <cstdint>
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <array>
#include <limits>
#include <string>
#include <string_view>
#include <vector>

#include "contracts/commands.h"
#include "contracts/character_state.h"
#include "contracts/game_ports/character_extras_port.h"
#include "contracts/game_ports/move_merge_port.h"
#include "contracts/game_ports/item_port.h"
#include "contracts/game_ports/item_dice_write_port.h"
#include "contracts/game_ports/item_money_write_port.h"
#include "contracts/game_ports/item_write_port.h"
#include "contracts/game_ports/item_inventory_state_write_port.h"
#include "contracts/http.h"
#include "contracts/inventory_action.h"
#include "contracts/inventory_refresh.h"
#include "contracts/inventory_slot.h"
#include "contracts/item_facts.h"
#include "contracts/item_state.h"
#include "contracts/move_merge.h"
#include "contracts/world_facts.h"
#include "usecases/item_slot.h"
#include "usecases/item_action.h"
#include "usecases/sell_item.h"
#include "hooks/merge_hook.h"
#include "http/json_lite.h"
#include "http/response.h"
#include "bags/snapshot_sdk.h"

#include "plugins/core.item/api/bag_source.h"
#include "plugins/core.item/runtime/core_item_internal.h"

#if !defined(QOL_CORE_ITEM_BAG_SOURCE_SEPARATE)
// 宿主测试直接编译 plugin.cpp，不读取插件 CMake 源清单；插件 target 单独编译该实现。
#include "plugins/core.item/api/bag_source.cpp"
#endif

namespace qol::features::core_item {

const contracts::CapabilityProvisionRef kProvides[] = {
    {contracts::kItemFactsCapabilityId, contracts::kItemFactsVersion, &kItemFactsApi},
    {contracts::kItemStateCapabilityId, contracts::kItemStateVersion, &kItemStateApi},
    {contracts::kBagSourceCapabilityId, contracts::kBagSourceVersion, &kBagSourceRegistryApi},
    {contracts::kInventoryRefreshCapabilityId,
     {contracts::kInventoryRefreshVersionMajor, contracts::kInventoryRefreshVersionMinor},
     &inventory_hooks::kInventoryRefreshApi},
    {contracts::kInventoryActionCapabilityId,
     {contracts::kInventoryActionVersionMajor, contracts::kInventoryActionVersionMinor},
     &kInventoryActionApi},
    {contracts::kInventorySlotCapabilityId, contracts::kInventorySlotVersion,
     &inventory_hooks::kInventorySlotApi},
    {contracts::kMoveMergeControlCapabilityId,
     {contracts::kMoveMergeControlVersionMajor, contracts::kMoveMergeControlVersionMinor},
     &merge::kMoveMergeControlApi},
};

const contracts::QolPortRequirement kPorts[] = {
    {contracts::QolPortId::QOL_PORT_ITEM,
     {contracts::kQolPortItemVersion.major, contracts::kQolPortItemVersion.minor}},
    {contracts::QolPortId::QOL_PORT_ITEM_MONEY_WRITE,
     {contracts::kQolPortItemMoneyWriteVersion.major,
      contracts::kQolPortItemMoneyWriteVersion.minor}},
    {contracts::QolPortId::QOL_PORT_ITEM_DICE_WRITE,
     {contracts::kQolPortItemDiceWriteVersion.major,
      contracts::kQolPortItemDiceWriteVersion.minor}},
    {contracts::QolPortId::QOL_PORT_ITEM_INVENTORY_STATE_WRITE,
     {contracts::kQolPortItemInventoryStateWriteVersion.major,
      contracts::kQolPortItemInventoryStateWriteVersion.minor}},
    {contracts::QolPortId::QOL_PORT_MOVE_MERGE,
     {contracts::kQolMoveMergePortVersion.major, contracts::kQolMoveMergePortVersion.minor}},
    {contracts::QolPortId::QOL_PORT_CHARACTER_EXTRAS,
     {contracts::kQolPortCharacterExtrasVersion.major,
      contracts::kQolPortCharacterExtrasVersion.minor}},
};

const contracts::CapabilityRequirement kOptionalRequires[] = {
    {contracts::kCharacterStateCapabilityId,
     {contracts::kCharacterStateVersion.major, contracts::kCharacterStateVersion.minor}},
    {contracts::kHttpRegistryCapabilityId,
     {contracts::kHttpRegistryVersion.major, contracts::kHttpRegistryVersion.minor}},
    {contracts::kHttpDispatchCapabilityId,
     {contracts::kHttpDispatchVersion.major, contracts::kHttpDispatchVersion.minor}},
    {contracts::kWorldFactsCapabilityId,
     {contracts::kWorldFactsVersionMajor, contracts::kWorldFactsVersionMinor}},
    {contracts::kExtensionBagCapabilityId,
     {static_cast<uint16_t>(contracts::kExtensionBagVersionMajor),
      contracts::kExtensionBagSaleRemoveVersionMinor}},
    {contracts::kInventoryMergeProjectionCapabilityId,
     {contracts::kInventoryMergeProjectionVersionMajor,
      contracts::kInventoryMergeProjectionVersionMinor}},
};

const contracts::PluginDescriptor& plugin_descriptor() {
    initialize_http_endpoints();
    static const contracts::PluginDescriptor descriptor = [] {
        uint32_t resource_count = 0;
        const contracts::ResourceClaim* resources = combined_resources(&resource_count);
        contracts::PluginDescriptor descriptor{
            kPluginId,
            kPluginVersion,
            0,
            nullptr,
            static_cast<uint32_t>(sizeof(kOptionalRequires) / sizeof(kOptionalRequires[0])),
            kOptionalRequires,
            static_cast<uint32_t>(sizeof(kProvides) / sizeof(kProvides[0])),
            kProvides,
            resource_count,
            resources,
            0,
            nullptr,
            &plugin_install,
            &plugin_deactivate,
            &plugin_on_enabled,
            &plugin_on_disabled,
            &plugin_fill_status,
            &on_resource_ready,
            kPorts,
            6,
            nullptr,
            0,
            kFeatures,
            2,
            kHttpEndpoints,
            static_cast<uint32_t>(kHttpEndpointCount),
            nullptr,
            kHttpRouteMetadata,
            kHttpRouteMetadataCount,
        };
        descriptor.deactivate_allowed = false;
        return descriptor;
    }();
    return descriptor;
}


}  // namespace qol::features::core_item
