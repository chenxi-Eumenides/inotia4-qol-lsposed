#include "plugin.h"

#include <atomic>
#include <cstring>

#include "../../contracts/events.h"
#include "../../contracts/attribute_quality.h"
#include "../../contracts/extension_bag.h"
#include "../../contracts/inventory_action.h"
#include "../../contracts/ownership.h"
#include "../../contracts/panels.h"
#include "../../contracts/world_facts.h"
#include "../../contracts/item_facts.h"
#include "../../contracts/slot_data.h"
#include "../../contracts/game_ports/inventory_extras_port.h"
#include "config.h"
#include "log.h"
#include "scan.h"
#include "store.h"
#include "ui/panel.h"

// 自动出售插件：门禁来源为配置键 `plugin.autosell.enabled`
// （nativeSetAutoSellEnabled → kCommandSetConfigBool）。门禁经 kEventConfigChanged 订阅驱动；
// install/on_enabled 时按配置初值应用一次。
//
// 资源（2 条，均常驻 gate=nullptr）：
//   · patch:SCENE_DRAW_EQUIP+0x1cc（kCallPatch/kFailClosed）：背包页入口按钮绘制宿主；
//   · own:popup.autosell（kOwnership/kFailSoft）：PopupState 死条目 F_PANEL_UNK3_ENTER 占用。
// UI（入口按钮 + 面板）为插件自有 ui/panel.*，provider 登记到 infra/panels。
//
// 过渡依赖：save_enter/save_exit 事件仅维护扫描活动态；配置持久化经 save.slot_data；
// 出售副作用由 inventory_hooks 提供的 inventory.action capability 承载。

namespace qol::features::autosell {
namespace {

constexpr const char* kPluginId = "autosell";
constexpr const char* kPluginVersion = "0.1.0";
constexpr const char* kGateKey = "plugin.autosell.enabled";

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
};

// extension_bag 的能力只在插件进入 enabled 时注册（其资源受配置门禁），故不能作为 required：
// 否则配置关闭或启动早期就会把整个 autosell 判为 blocked。运行期按需查找，缺失时走降级路径。
const contracts::CapabilityRequirement kOptionalRequires[] = {
    {contracts::kExtensionBagCapabilityId,
     {contracts::kExtensionBagVersionMajor, contracts::kExtensionBagVersionMinor}},
};

const contracts::QolPortRequirement kPorts[] = {
    {contracts::QolPortId::QOL_PORT_INVENTORY_EXTRAS,
     {contracts::kQolPortInventoryExtrasVersion.major,
      contracts::kQolPortInventoryExtrasVersion.minor}},
};

uint64_t g_config_subscription = 0;
const contracts::QolHostSdk* g_sdk = nullptr;
const contracts::QolGameHost* g_game = nullptr;
const contracts::WorldFactsApi* g_world_facts = nullptr;
const contracts::ItemFactsApi* g_item_facts = nullptr;
const contracts::QolInventoryExtrasPort* g_inventory_extras = nullptr;
const contracts::QolSlotDataApi* g_slot_data_api = nullptr;
std::atomic<bool> g_gate{false};

// 游戏内面板 provider：由 core.ui 容器经 ui_panels 打开/查询（panel_id="autosell"）。
const contracts::PanelProviderSpec kPanelProvider = {
    "autosell",
    kPluginId,
    &autosell_provider_open,
    &autosell_provider_close,
    &autosell_provider_active,
    &autosell_provider_block_reason,
};

// own:popup.autosell 的 OwnershipOps：acquire/release 走 hooks/popup_state 原语。
const contracts::OwnershipOps kPopupOwnershipOps{
    &autosell_popup_acquire,
    &autosell_popup_release,
};

// 2 条声明式资源（见文件头注释）：入口 call patch + PopupState 死条目占用。
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
    {
        "own:popup.autosell",  // id
        "own:popup.autosell",  // symbol（kOwnership 不使用站点解析，保留 id 作诊断标识）
        contracts::ResourceMechanism::kOwnership,
        contracts::ResourcePolicy::kPrivate,
        contracts::ResourceRequirement::kRequired,
        contracts::ResourceFailure::kFailSoft,
        true,                  // resumable
        kPluginId,             // owner
        "面板以 PopupState 死条目 F_PANEL_UNK3_ENTER 承载，需整段占用 5 回调并承诺写回快照",  // reason
        nullptr,               // gate（常驻）
        120,                   // step_budget
        reinterpret_cast<void*>(const_cast<contracts::OwnershipOps*>(&kPopupOwnershipOps)),  // handler
    },
};

// 门禁应用：同步扫描任务「已武装」标志与 UI 入口按钮/面板门控（现行为不变）。
void apply_gate(bool enabled) {
    g_gate.store(enabled, std::memory_order_release);
    autosell_set_global_enabled(enabled);
    autosell_ui_set_enabled(enabled);
}

void on_config_changed(void* ctx, const void* payload) {
    (void)ctx;
    if (payload == nullptr) return;
    const auto* event = static_cast<const contracts::ConfigChangedEvent*>(payload);
    if (event->key == nullptr || std::strcmp(event->key, kGateKey) != 0) return;
    apply_gate(event->int_value != 0);
}

bool config_enabled() {
    if (g_sdk == nullptr || g_sdk->config.get_bool == nullptr) return false;
    int32_t value = 0;
    if (g_sdk->config.get_bool(kGateKey, 0, &value).ok != kQolOk) return false;
    return value != 0;
}

int plugin_install(contracts::PluginContext& ctx) {
    g_sdk = ctx.sdk;
    g_game = ctx.game;
    g_inventory_extras = nullptr;
    g_world_facts = nullptr;
    g_item_facts = nullptr;
    if (ctx.ports == nullptr || ctx.port_count != 1 || ctx.ports[0] == nullptr) return 1;
    g_inventory_extras = static_cast<const contracts::QolInventoryExtrasPort*>(ctx.ports[0]);
    if (g_sdk == nullptr || g_sdk->config.get_bool == nullptr ||
        g_sdk->events.subscribe == nullptr || g_sdk->events.unsubscribe == nullptr ||
        g_sdk->panels.register_provider == nullptr ||
        g_sdk->panels.unregister_owner == nullptr) {
        return 1;
    }
    const void* iface = nullptr;
    if (g_sdk->capabilities.require == nullptr ||
        !contracts::succeeded(g_sdk->capabilities.require(
            contracts::kWorldFactsCapabilityId,
            {contracts::kWorldFactsVersionMajor, contracts::kWorldFactsVersionMinor}, &iface)) ||
        iface == nullptr) {
        g_sdk = nullptr;
        g_game = nullptr;
        return 1;
    }
    g_world_facts = static_cast<const contracts::WorldFactsApi*>(iface);
    iface = nullptr;
    if (!contracts::succeeded(g_sdk->capabilities.require(
            contracts::kItemFactsCapabilityId,
            {contracts::kItemFactsVersionMajor, contracts::kItemFactsVersionMinor}, &iface)) ||
        iface == nullptr) {
        g_sdk = nullptr;
        g_game = nullptr;
        g_world_facts = nullptr;
        return 1;
    }
    g_item_facts = static_cast<const contracts::ItemFactsApi*>(iface);
    if (!contracts::item_facts_api_size_valid(g_item_facts)) {
        g_sdk = nullptr;
        g_game = nullptr;
        g_world_facts = nullptr;
        g_item_facts = nullptr;
        return 1;
    }
    iface = nullptr;
    if (!contracts::succeeded(g_sdk->capabilities.require(
        contracts::kQolSlotDataCapabilityId,
        {contracts::kQolSlotDataVersion.major, contracts::kQolSlotDataVersion.minor}, &iface)) ||
        iface == nullptr ||
        !autosell_store_register(ctx.owner_token,
            static_cast<const contracts::QolSlotDataApi*>(iface))) {
        g_sdk = nullptr;
        g_game = nullptr;
        g_world_facts = nullptr;
        g_item_facts = nullptr;
        return 1;
    }
    g_slot_data_api = static_cast<const contracts::QolSlotDataApi*>(iface);
    // save_enter/save_exit 回调设施是共享过渡依赖（B4 迁移）；注册对同一 fn+ctx 幂等，
    // activate 重入安全。
    autosell_register_save_enter();
    autosell_register_save_exit();
    // 门禁初值取配置；订阅配置变更（重复订阅会留孤儿订阅，已订阅则跳过）。
    apply_gate(config_enabled());
    if (g_config_subscription == 0) {
        const QolResult result = g_sdk->events.subscribe(contracts::kEventConfigChanged,
                                                         &on_config_changed, nullptr,
                                                         &g_config_subscription);
        if (result.ok != kQolOk) return 1;
    }
    // 自身初始化：资源由宿主声明式安装；本面板登记 provider 供 core.ui 容器打开。
    if (g_sdk->panels.register_provider(&kPanelProvider).ok != kQolOk) return 1;
    return 0;
}

void plugin_deactivate(contracts::PluginContext& ctx) {
    autosell_store_unregister();
    g_slot_data_api = nullptr;
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
    if (g_config_subscription != 0) {
        if (g_sdk != nullptr && g_sdk->events.unsubscribe != nullptr) {
            (void)g_sdk->events.unsubscribe(g_config_subscription);
        }
        g_config_subscription = 0;
    }
    autosell_unregister_save_callbacks();
    g_game = nullptr;
    g_inventory_extras = nullptr;
    g_world_facts = nullptr;
    g_item_facts = nullptr;
    apply_gate(false);
    if (g_sdk != nullptr && g_sdk->panels.unregister_owner != nullptr) {
        (void)g_sdk->panels.unregister_owner(kPluginId);
    }
}

// 有资源（2 条常驻）但无资源 gate：启停只切业务门禁与 UI 门控；资源物理装卸由宿主负责。
void plugin_on_enabled(contracts::PluginContext& ctx) {
    if (g_slot_data_api != nullptr &&
        !autosell_store_register(ctx.owner_token, g_slot_data_api)) {
        AUTOSELL_LOG_WARN("slot data re-register on enable failed");
    }
    apply_gate(config_enabled());
}

void plugin_on_disabled(contracts::PluginContext& ctx) {
    (void)ctx;
    autosell_store_unregister();
    apply_gate(false);
}

void plugin_fill_status(contracts::PluginStatusOut& out) {
    out.enabled = g_gate.load(std::memory_order_acquire);
    out.error = nullptr;
}

const contracts::PluginDescriptor kDescriptor = {
    kPluginId,
    kPluginVersion,
    5, kRequires,  // requires=attribute.quality + inventory.action + world.facts + item.facts + save.slot_data
    1, kOptionalRequires,  // optional=extension_bag（门禁提供者，缺失走降级）
    0, nullptr,   // provides
    2, kResources,// resources（入口 call patch + own:popup.autosell）
    0, nullptr,   // jni（JNI 入口由 bridge/native/gamebridge_autosell.cpp 保留）
    &plugin_install,
    &plugin_deactivate,
    &plugin_on_enabled,
    &plugin_on_disabled,
    &plugin_fill_status,
    nullptr,      // on_resource_ready（kCallPatch/kOwnership 无 backup）
    kPorts,
    1,
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

const contracts::QolInventoryExtrasPort* inventory_extras() {
    return g_inventory_extras;
}

}  // namespace qol::features::autosell
