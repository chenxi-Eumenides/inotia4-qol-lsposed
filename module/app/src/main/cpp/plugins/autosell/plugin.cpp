#include "plugin.h"

#include <atomic>
#include <cstdio>
#include <cstring>

#include "../../contracts/attribute_quality.h"
#include "../../contracts/bag.h"
#include "../../contracts/events.h"
#include "../../contracts/inventory_action.h"
#include "../../contracts/inventory_access.h"
#include "../../contracts/panels.h"
#include "../../contracts/world_facts.h"
#include "../../contracts/item_facts.h"
#include "../../contracts/slot_data.h"
#include "../../contracts/ui.h"
#include "../../contracts/game_ports/inventory_extras_port.h"
#include "config.h"
#include "log.h"
#include "scan.h"
#include "store.h"
#include "ui/panel_internal.h"

// 自动出售插件：唯一开关为 descriptor feature `autosell.auto_sell`；插件仅在
// feature 开启（want_run）时安装，install/on_enabled 应用启用态、on_disabled/deactivate
// 收回。per-save-slot 筛选配置仍走 save.slot_data，不进 module config。
//
// 资源（1 条，常驻 gate=nullptr）：
//   · patch:SCENE_DRAW_EQUIP+0x1cc（kCallPatch/kFailClosed）：背包页入口按钮绘制宿主；
// 面板通过 ui_panels 注册 provider，由 core.custom_ui 的 PopupHost 托管回调与 popup 生命周期。
// UI（入口按钮 + 面板）为插件自有 ui/panel.*。
//
// 依赖：save_enter/save_exit 事件仅维护扫描活动态；配置持久化经 save.slot_data；
// 出售副作用由 inventory_hooks 提供的 inventory.action 承载；扩展袋由 core.item 的
// inventory.bag_source 枚举；原版和 C18 原生袋由 item port 扫描。

namespace qol::features::autosell {
namespace {

constexpr const char* kPluginId = "autosell";
constexpr const char* kPluginVersion = "0.1.0";

const contracts::CapabilityRequirement kRequires[] = {
    {contracts::kAttributeQualityCapabilityId,
     {contracts::kAttributeQualityVersionMajor, contracts::kAttributeQualityVersionMinor}},
    {contracts::kInventoryActionCapabilityId,
     {contracts::kInventoryActionVersionMajor, contracts::kInventoryActionVersionMinor}},
    {contracts::kWorldFactsCapabilityId,
     {contracts::kWorldFactsVersionMajor, contracts::kWorldFactsVersionMinor}},
    {contracts::kItemFactsCapabilityId,
     {contracts::kItemFactsVersionMajor, contracts::kItemFactsVersionMinor}},
    {contracts::kQolSlotDataCapabilityId,
     {contracts::kQolSlotDataVersion.major, contracts::kQolSlotDataVersion.minor}},
    {contracts::kUiStateCapabilityId,
     {contracts::kUiStateVersionMajor, contracts::kUiStateVersionMinor}},
};

// optional=4: http.registry + ui_panels + inventory.bag_source + inventory.access
const contracts::CapabilityRequirement kOptionalRequires[] = {
    {contracts::kHttpRegistryCapabilityId,
     {contracts::kHttpRegistryVersion.major, contracts::kHttpRegistryVersion.minor}},
    {contracts::kUiPanelsCapabilityId,
     {contracts::kUiPanelsVersionMajor, contracts::kUiPanelsVersionMinor}},
    {contracts::kBagSourceCapabilityId,
     {contracts::kBagSourceVersion.major, contracts::kBagSourceVersion.minor}},
    {contracts::kInventoryAccessCapabilityId,
     {contracts::kInventoryAccessVersion.major, contracts::kInventoryAccessVersion.minor}},
};

#define QOL_HTTP_ROUTE(method, path, owner, op_gated, lease_required) \
    {contracts::HttpMethod::method, path, owner, op_gated, lease_required},
const contracts::HttpRouteMetadataDescriptor kHttpRouteMetadata[] = {
#include "routes.inc"
};
#undef QOL_HTTP_ROUTE

const contracts::QolPortRequirement kPorts[] = {
    {contracts::QolPortId::QOL_PORT_INVENTORY_EXTRAS,
     {contracts::kQolPortInventoryExtrasVersion.major,
       contracts::kQolPortInventoryExtrasVersion.minor}},
    {contracts::QolPortId::QOL_PORT_ITEM,
     {contracts::kQolPortItemVersion.major, contracts::kQolPortItemVersion.minor}},
};

const contracts::QolHostSdk* g_sdk = nullptr;
const contracts::QolGameHost* g_game = nullptr;
const contracts::WorldFactsApi* g_world_facts = nullptr;
const contracts::ItemFactsApi* g_item_facts = nullptr;
const contracts::QolInventoryAccessApi* g_inventory_access = nullptr;
const contracts::UiStateApi* g_ui_state = nullptr;

void register_ui_state() {
    if (g_ui_state == nullptr || g_ui_state->register_state == nullptr) return;
    contracts::QolUiStateRegistration registration{};
    registration.struct_size = sizeof(registration);
    registration.type = contracts::kUiStateTypePanel;
    std::snprintf(registration.key, sizeof(registration.key), "%s", kPluginId);
    std::snprintf(registration.plugin_id, sizeof(registration.plugin_id), "%s", kPluginId);
    (void)g_ui_state->register_state(&registration);
}

void unregister_ui_state() {
    if (g_ui_state != nullptr && g_ui_state->unregister_state != nullptr) {
        (void)g_ui_state->unregister_state(contracts::kUiStateTypePanel, kPluginId);
    }
}

// ui_components 是可选能力：登记/撤销时宿主广播能力变更，插件撤销后只清缓存，
// 由下一次 autosell_components() 懒加载重新 require。
uint64_t g_capability_event_subscription = 0;

void on_capability_changed(void*, const void* payload) {
    if (payload == nullptr) return;
    const auto* event = static_cast<const contracts::CapabilityChangedEvent*>(payload);
    if (event->id == nullptr ||
        std::strcmp(event->id, contracts::kUiComponentsCapabilityId) != 0) {
        return;
    }
    if (event->available == 0) autosell_invalidate_ui_components();
}

void subscribe_capability_changes() {
    if (g_sdk == nullptr || g_sdk->events.subscribe == nullptr) return;
    if (g_capability_event_subscription != 0) return;
    uint64_t subscription = 0;
    if (contracts::succeeded(g_sdk->events.subscribe(contracts::kEventCapabilityChanged,
                                                     &on_capability_changed, nullptr,
                                                     &subscription))) {
        g_capability_event_subscription = subscription;
    }
}

void unsubscribe_capability_changes() {
    if (g_capability_event_subscription == 0) return;
    if (g_sdk != nullptr && g_sdk->events.unsubscribe != nullptr) {
        (void)g_sdk->events.unsubscribe(g_capability_event_subscription);
    }
    g_capability_event_subscription = 0;
}
const contracts::QolInventoryExtrasPort* g_inventory_extras = nullptr;
const contracts::QolItemPort* g_item_port = nullptr;
const contracts::QolSlotDataApi* g_slot_data_api = nullptr;
std::atomic<bool> g_gate{false};

// 游戏内面板 provider：由 core.ui 容器经 ui_panels 打开/查询（panel_id="autosell"）。
const contracts::PanelCallbacks kPanelCallbacks{
    &autosell_panel_enter,
    &autosell_panel_process,
    &autosell_panel_f3,
    &autosell_panel_f4,
    &autosell_panel_event,
};

const contracts::PanelProviderSpec kPanelProvider = {
    "autosell",
    kPluginId,
    &autosell_provider_open,
    &autosell_provider_close,
    &autosell_provider_active,
    &autosell_provider_block_reason,
    &kPanelCallbacks,
};

// 入口按钮绘制宿主由声明式 call patch 提供；PopupState 由 core.custom_ui 的 PopupHost 托管。
const contracts::ResourceClaim kResources[] = {
    {
        "patch:SCENE_DRAW_EQUIP+0x1cc",  // id
        "P_SCENE_DRAW_EQUIP_DESC",       // symbol（game/access/patches 站点名）
        contracts::ResourceMechanism::kCallPatch,
        contracts::ResourcePolicy::kPrivate,
        contracts::ResourceRequirement::kRequired,
        contracts::ResourceFailure::kFailClosed,
        true,                            // resumable
        kPluginId,                       // owner
        "背包页入口按钮绘制宿主：+0x1cc 原 bl UIDesc_Draw 改指模块 wrapper",  // reason
        nullptr,                         // gate（常驻）
        120,                             // step_budget
        reinterpret_cast<void*>(&autosell_equip_desc_draw_wrapper),  // handler
    },
};

// 门禁应用：同步扫描任务「已武装」标志与 UI 入口按钮/面板门控（现行为不变）。
void apply_gate(bool enabled) {
    g_gate.store(enabled, std::memory_order_release);
    autosell_set_global_enabled(enabled);
    autosell_ui_set_enabled(enabled);
}

void clear_runtime_capabilities() {
    g_slot_data_api = nullptr;
    g_world_facts = nullptr;
    g_item_facts = nullptr;
    g_inventory_access = nullptr;
    g_ui_state = nullptr;
}

bool acquire_runtime_capabilities(contracts::PluginContext& ctx) {
    clear_runtime_capabilities();
    if (ctx.sdk == nullptr || ctx.sdk->capabilities.require == nullptr) return false;

    const void* iface = nullptr;
    if (!contracts::succeeded(ctx.sdk->capabilities.require(
            contracts::kWorldFactsCapabilityId,
            {contracts::kWorldFactsVersionMajor, contracts::kWorldFactsVersionMinor}, &iface)) ||
        iface == nullptr) {
        return false;
    }
    const auto* world_facts = static_cast<const contracts::WorldFactsApi*>(iface);

    iface = nullptr;
    if (!contracts::succeeded(ctx.sdk->capabilities.require(
            contracts::kItemFactsCapabilityId,
            {contracts::kItemFactsVersionMajor, contracts::kItemFactsVersionMinor}, &iface)) ||
        iface == nullptr) {
        return false;
    }
    const auto* item_facts = static_cast<const contracts::ItemFactsApi*>(iface);
    if (!contracts::item_facts_api_size_valid(item_facts)) return false;

    iface = nullptr;
    if (!contracts::succeeded(ctx.sdk->capabilities.require(
            contracts::kQolSlotDataCapabilityId,
            {contracts::kQolSlotDataVersion.major, contracts::kQolSlotDataVersion.minor},
            &iface)) ||
        iface == nullptr) {
        return false;
    }
    const auto* slot_data_api = static_cast<const contracts::QolSlotDataApi*>(iface);

    iface = nullptr;
    if (!contracts::succeeded(ctx.sdk->capabilities.require(
            contracts::kUiStateCapabilityId,
            {contracts::kUiStateVersionMajor, contracts::kUiStateVersionMinor}, &iface)) ||
        iface == nullptr) {
        return false;
    }
    const auto* ui_state = static_cast<const contracts::UiStateApi*>(iface);
    if (ui_state->read_layers == nullptr) return false;

    iface = nullptr;
    const contracts::QolInventoryAccessApi* inventory_access = nullptr;
    if (contracts::succeeded(ctx.sdk->capabilities.require(
            contracts::kInventoryAccessCapabilityId,
            {contracts::kInventoryAccessVersion.major, contracts::kInventoryAccessVersion.minor},
            &iface)) &&
        iface != nullptr) {
        const auto* candidate = static_cast<const contracts::QolInventoryAccessApi*>(iface);
        if (contracts::qol_inventory_native_access_api_valid(candidate)) inventory_access = candidate;
    }

    g_world_facts = world_facts;
    g_item_facts = item_facts;
    g_slot_data_api = slot_data_api;
    g_ui_state = ui_state;
    g_inventory_access = inventory_access;
    if (!autosell_store_register(ctx.owner_token, slot_data_api)) {
        clear_runtime_capabilities();
        return false;
    }
    return true;
}

int plugin_install(contracts::PluginContext& ctx) {
    // 重新 install 可能未经过 deactivate；先撤销旧订阅再覆盖 sdk，避免订阅泄漏。
    unsubscribe_capability_changes();
    g_sdk = ctx.sdk;
    g_game = ctx.game;
    g_inventory_extras = nullptr;
    g_item_port = nullptr;
    clear_runtime_capabilities();
    if (ctx.ports == nullptr || ctx.port_count != 2 || ctx.ports[0] == nullptr ||
        ctx.ports[1] == nullptr) {
        g_sdk = nullptr;
        g_game = nullptr;
        return 1;
    }
    g_inventory_extras = static_cast<const contracts::QolInventoryExtrasPort*>(ctx.ports[0]);
    g_item_port = static_cast<const contracts::QolItemPort*>(ctx.ports[1]);
    if (g_sdk == nullptr || g_sdk->panels.register_provider == nullptr ||
        g_sdk->panels.unregister_owner == nullptr) {
        g_sdk = nullptr;
        g_game = nullptr;
        g_inventory_extras = nullptr;
        g_item_port = nullptr;
        return 1;
    }
    if (!acquire_runtime_capabilities(ctx)) {
        g_sdk = nullptr;
        g_game = nullptr;
        g_inventory_extras = nullptr;
        g_item_port = nullptr;
        return 1;
    }
    subscribe_capability_changes();
    register_ui_state();
    // save_enter/save_exit 事件订阅：注册对同一 fn+ctx 幂等，activate 重入安全。
    autosell_register_save_enter();
    autosell_register_save_exit();
    // feature 为插件唯一开关：want_run 成立才会 install，故进入此路径即视为启用。
    apply_gate(true);
    // 自身初始化：资源由宿主声明式安装；本面板登记 provider 供 core.ui 容器打开。
    if (g_sdk->panels.register_provider(&kPanelProvider).ok != kQolOk) {
        unsubscribe_capability_changes();
        g_item_port = nullptr;
        return 1;
    }
    return 0;
}

void plugin_deactivate(contracts::PluginContext& ctx) {
    unsubscribe_capability_changes();
    autosell_store_unregister();
    unregister_ui_state();
    clear_runtime_capabilities();
    if (ctx.sdk != nullptr && ctx.sdk->capabilities.require != nullptr) {
        const void* iface = nullptr;
        if (contracts::succeeded(ctx.sdk->capabilities.require(
                contracts::kUiPanelsCapabilityId,
                {contracts::kUiPanelsVersionMajor, contracts::kUiPanelsVersionMinor}, &iface)) &&
            iface != nullptr) {
            const auto* panels = static_cast<const contracts::UiPanelsApi*>(iface);
            if (panels->deactivate_owner != nullptr) (void)panels->deactivate_owner(kPluginId);
        }
    }
    autosell_unregister_save_callbacks();
    g_game = nullptr;
    g_inventory_extras = nullptr;
    g_item_port = nullptr;
    apply_gate(false);
    if (g_sdk != nullptr && g_sdk->panels.unregister_owner != nullptr) {
        (void)g_sdk->panels.unregister_owner(kPluginId);
    }
}

// 有资源（2 条常驻）但无资源 gate：启停只切业务门禁与 UI 门控；资源物理装卸由宿主负责。
void plugin_on_enabled(contracts::PluginContext& ctx) {
    if (!acquire_runtime_capabilities(ctx)) {
        AUTOSELL_LOG_WARN("runtime capability acquisition on enable failed");
        apply_gate(false);
        return;
    }
    register_ui_state();
    apply_gate(true);
}

void plugin_on_disabled(contracts::PluginContext& ctx) {
    (void)ctx;
    autosell_store_unregister();
    unregister_ui_state();
    clear_runtime_capabilities();
    apply_gate(false);
}

void plugin_fill_status(contracts::PluginStatusOut& out) {
    out.enabled = g_gate.load(std::memory_order_acquire);
    out.error = nullptr;
}

// Feature callback does not replace the existing lifecycle or business controls.
constexpr contracts::FeatureDefinition kFeatures[] = {
    {"autosell.auto_sell", 0, contracts::FeatureCategory::kGameplay,
      contracts::FeatureType::kToggle, "自动出售", "按照规则自动出售物品",
      [](contracts::PluginContext&, uint8_t) -> QolResult { return contracts::ok(); }, nullptr,
      nullptr, -1},
};

const contracts::PluginDescriptor kDescriptor = {
    kPluginId,
    kPluginVersion,
    6, kRequires,  // requires=6: attribute.quality + inventory.action + world.facts + item.facts + save.slot_data + ui.state
    static_cast<uint32_t>(sizeof(kOptionalRequires) / sizeof(kOptionalRequires[0])),
    kOptionalRequires,  // optional=3: http.registry + ui_panels + inventory.bag_source
    0, nullptr,   // provides
    1, kResources,// resources（背包入口 call patch）
    0, nullptr,   // jni（JNI 入口由 bridge/native/gamebridge_autosell.cpp 保留）
    &plugin_install,
    &plugin_deactivate,
    &plugin_on_enabled,
    &plugin_on_disabled,
    &plugin_fill_status,
    nullptr,      // on_resource_ready（无 backup 交接）
    kPorts,
    2,             // ports=2
    nullptr, 0,
    kFeatures, 1,
    nullptr, 0,
    nullptr,
    kHttpRouteMetadata,
    static_cast<uint32_t>(sizeof(kHttpRouteMetadata) / sizeof(kHttpRouteMetadata[0])),
};

}  // namespace

const contracts::PluginDescriptor& plugin_descriptor() {
    return kDescriptor;
}

const contracts::QolHostSdk* host_sdk() {
    return g_sdk;
}

const contracts::QolGameHost* game_host() {
    return g_game;
}

const contracts::WorldFactsApi* world_facts() {
    return g_world_facts;
}

const contracts::ItemFactsApi* item_facts() {
    return g_item_facts;
}

const contracts::UiStateApi* ui_state() {
    return g_ui_state;
}

const contracts::QolInventoryExtrasPort* inventory_extras() {
    return g_inventory_extras;
}

const contracts::QolItemPort* item_port() {
    return g_item_port;
}

const contracts::QolInventoryAccessApi* inventory_access_api() {
    return g_inventory_access;
}

}  // namespace qol::features::autosell
