#include "plugin.h"
#include <atomic>
#include "../../contracts/ui.h"
#include "log.h"

namespace qol::features::item_lab {
namespace {
constexpr char kId[] = "item_lab";
std::atomic<bool> g_enabled{false};
const contracts::QolHostSdk* g_sdk = nullptr;
const contracts::InventoryActionApi* g_action = nullptr;
const contracts::ItemPayloadApi* g_payload = nullptr;
const contracts::QolInventoryAccessApi* g_access = nullptr;
const contracts::QolInventoryExtrasPort* g_extras = nullptr;
const contracts::CapabilityRequirement kRequires[] = {
    {contracts::kInventoryActionCapabilityId, {3, 4}},
    {contracts::kItemPayloadCapabilityId, {1, 1}},
    {contracts::kInventoryAccessCapabilityId, {1, 1}},
    {contracts::kUiComponentsCapabilityId, {contracts::kUiContractMajor, contracts::kUiContractMinor}},
};
const contracts::QolPortRequirement kPorts[] = {
    {contracts::QolPortId::QOL_PORT_INVENTORY_EXTRAS, {1, 4}},
};
const contracts::ResourceClaim kResources[] = {{
    "patch:SCENE_DRAW_EQUIP+0x1d0", "P_SCENE_DRAW_EQUIP_ROLLDICE",
    contracts::ResourceMechanism::kCallPatch, contracts::ResourcePolicy::kPrivate,
    contracts::ResourceRequirement::kRequired, contracts::ResourceFailure::kFailClosed,
    true, kId, "背包页测试物品按钮绘制", nullptr, 120,
    reinterpret_cast<void*>(&item_lab_equip_roll_dice_draw_wrapper)}};
static_assert(kResourceCount == (sizeof(kResources) / sizeof(kResources[0])),
              "kResourceCount must match kResources");
int install(contracts::PluginContext& ctx) {
    g_sdk = ctx.sdk;
    g_extras = ctx.ports && ctx.port_count ? static_cast<const contracts::QolInventoryExtrasPort*>(ctx.ports[0]) : nullptr;
    if (!g_sdk || !g_sdk->capabilities.require || !g_extras) return 1;
    const void* api = nullptr;
    if (!contracts::succeeded(g_sdk->capabilities.require(contracts::kInventoryActionCapabilityId,{3,4},&api)) || !api) return 1;
    g_action = static_cast<const contracts::InventoryActionApi*>(api);
    api = nullptr;
    if (!contracts::succeeded(g_sdk->capabilities.require(contracts::kItemPayloadCapabilityId,{1,1},&api)) || !api) return 1;
    g_payload = static_cast<const contracts::ItemPayloadApi*>(api);
    api = nullptr;
    if (!contracts::succeeded(g_sdk->capabilities.require(contracts::kInventoryAccessCapabilityId,{1,1},&api)) || !api) return 1;
    g_access = static_cast<const contracts::QolInventoryAccessApi*>(api);
    g_enabled.store(true, std::memory_order_release);
    ITEM_LAB_INFO("install ok sdk=%p extras=%p action=%p payload=%p access=%p",
                  static_cast<const void*>(g_sdk), static_cast<const void*>(g_extras),
                  static_cast<const void*>(g_action), static_cast<const void*>(g_payload),
                  static_cast<const void*>(g_access));
    return 0;
}
void deactivate(contracts::PluginContext&) { g_enabled.store(false, std::memory_order_release); g_sdk=nullptr; g_action=nullptr; g_payload=nullptr; g_access=nullptr; g_extras=nullptr; }
void on_enabled(contracts::PluginContext&) { g_enabled.store(true, std::memory_order_release); }
void on_disabled(contracts::PluginContext&) { g_enabled.store(false, std::memory_order_release); }
void status(contracts::PluginStatusOut& out) { out.enabled = g_enabled.load(std::memory_order_acquire); out.error=nullptr; }
const contracts::FeatureDefinition kFeatures[] = {{"item_lab.construct", 1, contracts::FeatureCategory::kTools,
    contracts::FeatureType::kToggle, "物品构造测试", "背包页按钮一次构造固定测试物品",
    [](contracts::PluginContext&, uint8_t enabled) -> QolResult { g_enabled.store(enabled != 0, std::memory_order_release); return contracts::ok(); }, nullptr, nullptr, -1}};
const contracts::PluginDescriptor kDescriptor = {kId,"0.1.0",4,kRequires,0,nullptr,0,nullptr,
    1,kResources,0,nullptr,&install,&deactivate,&on_enabled,&on_disabled,&status,nullptr,
    kPorts,1,nullptr,0,kFeatures,1,nullptr,0,nullptr,nullptr,0};
}
const contracts::PluginDescriptor& plugin_descriptor(){return kDescriptor;}
const contracts::QolHostSdk* host_sdk(){return g_sdk;}
const contracts::InventoryActionApi* inventory_action(){return g_action;}
const contracts::ItemPayloadApi* item_payload(){return g_payload;}
const contracts::QolInventoryAccessApi* inventory_access(){return g_access;}
const contracts::QolInventoryExtrasPort* inventory_extras(){return g_extras;}
void item_lab_set_enabled(bool enabled){g_enabled.store(enabled,std::memory_order_release);}
bool item_lab_enabled(){return g_enabled.load(std::memory_order_acquire);}
}
