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
#include "contracts/item_payload.h"
#include "contracts/item_state.h"
#include "contracts/world_facts.h"
#include "usecases/item_slot.h"
#include "usecases/item_action.h"
#include "usecases/sell_item.h"
#include "http/json_lite.h"
#include "http/response.h"
#include "bags/snapshot_sdk.h"

#include "plugins/core.item/api/bag_source.h"
#include "plugins/core.item/api/item_payload.h"
#include "plugins/core.item/api/native_inventory.h"
#include "plugins/core.item/api/projection.h"
#include "plugins/core.item/runtime/core_item_internal.h"

#if !defined(QOL_CORE_ITEM_BAG_SOURCE_SEPARATE)
// 宿主测试直接编译 plugin.cpp，不读取插件 CMake 源清单；插件 target 单独编译该实现。
#include "plugins/core.item/api/bag_source.cpp"
#endif
#if !defined(QOL_CORE_ITEM_NATIVE_INVENTORY_SEPARATE)
#include "plugins/core.item/api/native_inventory.cpp"
#endif
#if !defined(QOL_CORE_ITEM_PROJECTION_SEPARATE)
#include "plugins/core.item/api/projection.cpp"
#endif
#if !defined(QOL_CORE_ITEM_ITEM_PAYLOAD_SEPARATE)
#include "plugins/core.item/api/item_payload.cpp"
#endif

namespace qol::features::core_item {

const contracts::CapabilityProvisionRef kProvides[] = {
    {contracts::kItemFactsCapabilityId, contracts::kItemFactsVersion, &kItemFactsApi},
    {contracts::kItemStateCapabilityId, contracts::kItemStateVersion, &kItemStateApi},
    {contracts::kInventoryActionCapabilityId,
     {contracts::kInventoryActionVersionMajor, contracts::kInventoryActionVersionMinor},
      &kInventoryActionApi},
    {contracts::kInventorySlotCapabilityId, contracts::kInventorySlotVersion,
     &inventory_hooks::kInventorySlotApi},
    {contracts::kInventoryProjectionCapabilityId,
     {contracts::kInventoryProjectionVersionMajor, contracts::kInventoryProjectionVersionMinor},
     &kInventoryProjectionRegistryApi},
    {contracts::kItemPayloadCapabilityId,
     {contracts::kItemPayloadVersionMajor, contracts::kItemPayloadVersionMinor},
     &item_payload_api()},
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
    // 袋源注册表由 core.inventory 提供；缺失时注册源查询为空，原生路径不受影响。
    {contracts::kBagSourceCapabilityId,
     {contracts::kBagSourceVersion.major, contracts::kBagSourceVersion.minor}},
    {contracts::kInventoryAccessCapabilityId,
     {contracts::kInventoryAccessVersion.major, contracts::kInventoryAccessVersion.minor}},
    // 原版刷新 trampoline 由 extension_bag 提供（owner 自本插件迁出）；缺失时出售结算
    // 后的刷新退化为 no-op，不影响其余行为。
    {contracts::kInventoryRefreshCapabilityId,
     {contracts::kInventoryRefreshVersionMajor, contracts::kInventoryRefreshVersionMinor}},
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
            5,
            nullptr,
            0,
            kFeatures,
            1,
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
