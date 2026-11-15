#include "plugin.h"

#include "../../contracts/panels.h"
#include "../../contracts/ui.h"
#include "../../contracts/game_ports/ports.h"
#include "../../contracts/game_ports/item_port.h"

#include <atomic>

namespace qol::features::item_editor {
namespace {
constexpr const char* kPluginId = "item_editor";
constexpr const char* kPanelId = "item_editor";
constexpr const char* kResourceId = "patch:SCENE_DRAW_EQUIP+0x1d0";
const contracts::QolHostSdk* g_sdk = nullptr;
const contracts::QolGameHost* g_game = nullptr;
const contracts::QolItemPort* g_item_port = nullptr;
contracts::QolOwnerToken g_owner_token{};
std::atomic<bool> g_feature_enabled{false};
std::atomic<bool> g_enabled{false};

bool op_enabled() {
    if (g_sdk == nullptr || g_sdk->security == nullptr || g_sdk->security->op_enabled == nullptr) return false;
    uint8_t enabled = 0;
    return contracts::succeeded(g_sdk->security->op_enabled(g_sdk->security->context, &enabled)) && enabled != 0;
}
const contracts::UiPanelsApi* panels_api() {
    if (g_sdk == nullptr || g_sdk->capabilities.require == nullptr) return nullptr;
    const void* iface = nullptr;
    if (!contracts::succeeded(g_sdk->capabilities.require(contracts::kUiPanelsCapabilityId,
            {contracts::kUiPanelsVersionMajor, contracts::kUiPanelsVersionMinor}, &iface))) return nullptr;
    return static_cast<const contracts::UiPanelsApi*>(iface);
}
void refresh_enabled() {
    g_enabled.store(g_feature_enabled.load(std::memory_order_acquire) && op_enabled(),
                    std::memory_order_release);
}

QolResult toggle(contracts::PluginContext&, uint8_t enabled) {
    if (enabled != 0 && !op_enabled()) {
        g_feature_enabled.store(false, std::memory_order_release);
        g_enabled.store(false, std::memory_order_release);
        return contracts::error(kQolErrBlocked, "需要开启高级权限", 24);
    }
    g_feature_enabled.store(enabled != 0, std::memory_order_release);
    refresh_enabled();
    return contracts::ok();
}
const contracts::QolPortRequirement kPorts[] = {
    {contracts::QolPortId::QOL_PORT_ITEM,
     {contracts::kQolPortItemVersion.major, contracts::kQolPortItemVersion.minor}},
};
const contracts::ResourceClaim kResources[] = {{
    kResourceId, "P_SCENE_DRAW_EQUIP_ROLLDICE", contracts::ResourceMechanism::kCallPatch,
    contracts::ResourcePolicy::kPrivate, contracts::ResourceRequirement::kRequired,
    contracts::ResourceFailure::kFailClosed, true, kPluginId,
    "背包页物品编辑器入口按钮绘制宿主", nullptr, 120,
    reinterpret_cast<void*>(&item_editor_equip_draw_wrapper),
}};
const contracts::PanelCallbacks kCallbacks{&panel_enter, &panel_process, &panel_f3, &panel_f4, &panel_event};
const contracts::PanelProviderSpec kProvider{
    kPanelId, kPluginId, &provider_open, &provider_close, &provider_active,
    &provider_block_reason, &kCallbacks};
int install(contracts::PluginContext& ctx) {
    if (ctx.sdk == nullptr || ctx.ports == nullptr || ctx.port_count != 1 || ctx.ports[0] == nullptr ||
        ctx.sdk->panels.register_provider == nullptr) return 1;
    g_sdk = ctx.sdk;
    g_game = ctx.game;
    g_item_port = static_cast<const contracts::QolItemPort*>(ctx.ports[0]);
    if (!contracts::qol_item_port_draw_icon_size_valid(g_item_port) ||
        !contracts::qol_item_port_class_index_size_valid(g_item_port) ||
        !contracts::qol_item_port_base_stats_size_valid(g_item_port) ||
        !contracts::qol_item_port_base_stats_ex_size_valid(g_item_port) ||
        !contracts::qol_item_port_drop_size_valid(g_item_port) ||
        g_item_port->read.item_class_index == nullptr ||
        g_item_port->read.detail_item_context == nullptr ||
        g_item_port->read.inventory_item_view == nullptr ||
        g_item_port->read.read_item_base_stats == nullptr ||
        g_item_port->read.read_item_base_stats_ex == nullptr ||
        g_item_port->read.draw_icon == nullptr) {
        g_item_port = nullptr;
        g_game = nullptr;
        g_sdk = nullptr;
        return 1;
    }
    if (!contracts::succeeded(g_sdk->panels.register_provider(&kProvider))) {
        g_item_port = nullptr;
        g_game = nullptr;
        g_sdk = nullptr;
        return 1;
    }
    g_owner_token = ctx.owner_token;
    return 0;
}
void deactivate(contracts::PluginContext& ctx) {
    g_feature_enabled.store(false, std::memory_order_release);
    g_enabled.store(false, std::memory_order_release);
    // 与自动出售一致：面板 owner 生命周期由 deactivate_owner 统一处理，
    // 不在停用路径里直接 close(panel_id)。
    const auto* panels = panels_api();
    if (panels != nullptr && panels->deactivate_owner != nullptr)
        (void)panels->deactivate_owner(kPluginId);
    if (ctx.sdk != nullptr && ctx.sdk->panels.unregister_owner != nullptr)
        (void)ctx.sdk->panels.unregister_owner(kPluginId);
    g_item_port = nullptr;
    g_game = nullptr;
    g_owner_token = {};
    g_sdk = nullptr;
}
void on_enabled(contracts::PluginContext&) {
    g_feature_enabled.store(true, std::memory_order_release);
    refresh_enabled();
}
void on_disabled(contracts::PluginContext&) {
    g_feature_enabled.store(false, std::memory_order_release);
    g_enabled.store(false, std::memory_order_release);
}
void status(contracts::PluginStatusOut& out) {
    out.enabled = g_enabled.load(std::memory_order_acquire);
    out.error = nullptr;
}
constexpr contracts::FeatureDefinition kFeatures[] = {{
    "item_editor.panel", 0, contracts::FeatureCategory::kTools,
    contracts::FeatureType::kToggle, "物品编辑器", "显示物品词条面板",
    &toggle, nullptr, nullptr, -1}};
const contracts::PluginDescriptor kDescriptor{
    kPluginId, "0.1.0", 0, nullptr, 0, nullptr, 0, nullptr,
    1, kResources, 0, nullptr, &install, &deactivate, &on_enabled, &on_disabled, &status,
    nullptr, kPorts, 1, nullptr, 0, kFeatures, 1};
}
const contracts::PluginDescriptor& plugin_descriptor() { return kDescriptor; }
const contracts::QolHostSdk* host_sdk() { return g_sdk; }
const contracts::QolGameHost* game_host() { return g_game; }
const contracts::QolItemPort* item_port() { return g_item_port; }
int32_t stack_limit_max_count() {
    const auto* api = contracts::feature_if_available(host_sdk());
    if (api == nullptr || api->read_feature == nullptr) return 99;
    const auto* control = contracts::feature_control_if_available(host_sdk());
    if (control == nullptr || control->read_owner_availability == nullptr) return 99;
    contracts::FeatureOwnerAvailability availability{};
    if (!contracts::succeeded(control->read_owner_availability(
            control->context, "stack_limit", &availability)) ||
        availability != contracts::FeatureOwnerAvailability::kAvailable) return 99;
    uint8_t enabled = 0;
    const QolResult result = api->read_feature(api->context,g_owner_token,
        "stack_limit","stack_limit.limit_999",&enabled);
    return contracts::succeeded(result) && enabled != 0 ? 999 : 99;
}
bool provider_open() { refresh_enabled(); return g_enabled.load(std::memory_order_acquire); }
void provider_close() { panel_destroy(); }
bool provider_active() { return panel_is_active(); }
const char* provider_block_reason() {
    if (!op_enabled()) return "需要开启高级权限";
    return g_enabled.load(std::memory_order_acquire) ? nullptr : "请先启用物品编辑器";
}
bool item_editor_enabled() {
    refresh_enabled();
    return g_enabled.load(std::memory_order_acquire);
}
void item_editor_force_disable() {
    g_feature_enabled.store(false, std::memory_order_release);
    g_enabled.store(false, std::memory_order_release);
    // OP 失效发生在主线程；仍走 owner 生命周期，不直接 close(panel_id)。
    const auto* panels = panels_api();
    if (panels != nullptr && panels->deactivate_owner != nullptr)
        (void)panels->deactivate_owner(kPluginId);
}
}
