#include "plugin.h"

#include "../../contracts/ui.h"
#include "../../contracts/ui_choice.h"
#include "../../contracts/game_ports/ports.h"
#include "../../contracts/game_ports/world_action_port.h"
#include "../../contracts/http.h"
#include "feature.h"

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>

namespace qol::features::world_teleport {
namespace {

constexpr const char* kPluginId = "world_teleport";
constexpr const char* kPluginVersion = "0.1.0";

const contracts::QolHostSdk* g_sdk = nullptr;
const contracts::QolWorldActionPort* g_world_action_port = nullptr;
const contracts::WorldFactsApi* g_world_facts = nullptr;
const contracts::ItemFactsApi* g_item_facts = nullptr;
bool g_installed = false;
const char* g_install_error = nullptr;

uint64_t (*g_backup_call_map_name)() = nullptr;

// 入口为 UIPlay_CallMapName 的函数级 kEntryHook（inventory_hooks 同构）：符号经 dynsym
// 解析（game_symbols.h 单一来源），原版/monster/C18 三版本同址，无指纹、无版本表。
// 不常规转发原函数：原函数尾部在改版上是 B→改版注入块，转发会连带触发改版自己的
// 传送对话框。原函数体不读任何入参寄存器（首指令即覆写 x0-x2），无参 wrapper 安全。
uint64_t world_teleport_call_map_name_wrapper() {
    if (g_backup_call_map_name == nullptr || !g_installed) {
        return g_backup_call_map_name != nullptr ? g_backup_call_map_name() : 1;
    }
    qol::features::world_teleport::world_teleport_call_map_name();
    return 1;  // 原版尾部语义 mov x0,#1
}

const contracts::ResourceClaim kResources[] = {
    {
        "hook:UIPlay_CallMapName",
        "F_UI_PLAY_CALL_MAP_NAME_VMA",
        contracts::ResourceMechanism::kEntryHook,
        contracts::ResourcePolicy::kPrivate,
        contracts::ResourceRequirement::kRequired,
        contracts::ResourceFailure::kFailClosed,
        true,
        kPluginId,
        "世界态地图边界传送选择面板（函数级入口接管）",
        nullptr,
        120,
        reinterpret_cast<void*>(&world_teleport_call_map_name_wrapper),
    },
};

const contracts::CapabilityRequirement kOptionalRequires[] = {
    {contracts::kUiChoiceCapabilityId,
     QolVersionRange{static_cast<uint16_t>(contracts::kUiChoiceVersionMajor),
                     static_cast<uint16_t>(contracts::kUiChoiceVersionMinor)}},
    {contracts::kUiStateCapabilityId,
     QolVersionRange{contracts::kUiStateVersionMajor, contracts::kUiStateVersionMinor}},
};

const contracts::CapabilityRequirement kRequires[] = {
    {contracts::kWorldFactsCapabilityId,
     {contracts::kWorldFactsVersionMajor, contracts::kWorldFactsVersionMinor}},
    {contracts::kItemFactsCapabilityId,
     {contracts::kItemFactsVersionMajor, contracts::kItemFactsVersionMinor}},
};

const contracts::QolPortRequirement kPorts[] = {
    {contracts::QolPortId::QOL_PORT_WORLD_ACTION,
     {contracts::kQolPortWorldActionVersion.major, contracts::kQolPortWorldActionVersion.minor}},
};

int plugin_install(contracts::PluginContext& ctx) {
    g_sdk = ctx.sdk;
    g_world_action_port = nullptr;
    g_world_facts = nullptr;
    g_item_facts = nullptr;
    g_installed = false;
    g_install_error = nullptr;
    if (ctx.struct_size < sizeof(contracts::PluginContext) ||
        ctx.sdk_major != contracts::kHostSdkMajor || g_sdk == nullptr ||
        ctx.ports == nullptr || ctx.port_count != 1 || ctx.ports[0] == nullptr) {
        g_sdk = nullptr;
        g_world_action_port = nullptr;
        g_world_facts = nullptr;
        g_item_facts = nullptr;
        g_install_error = "install failed: world teleport dependencies not ready";
        return 1;
    }
    g_world_action_port = static_cast<const contracts::QolWorldActionPort*>(ctx.ports[0]);
    const void* world_facts_iface = nullptr;
    if (g_sdk->capabilities.require == nullptr ||
        !contracts::succeeded(g_sdk->capabilities.require(
            contracts::kWorldFactsCapabilityId,
            {contracts::kWorldFactsVersionMajor, contracts::kWorldFactsVersionMinor},
            &world_facts_iface)) ||
        world_facts_iface == nullptr) {
        g_sdk = nullptr;
        g_world_action_port = nullptr;
        g_install_error = "install failed: world facts unavailable";
        return 1;
    }
    g_world_facts = static_cast<const contracts::WorldFactsApi*>(world_facts_iface);
    const void* item_facts_iface = nullptr;
    if (!contracts::succeeded(g_sdk->capabilities.require(
            contracts::kItemFactsCapabilityId,
            {contracts::kItemFactsVersionMajor, contracts::kItemFactsVersionMinor},
            &item_facts_iface)) ||
        item_facts_iface == nullptr || !contracts::item_facts_api_size_valid(
                                           static_cast<const contracts::ItemFactsApi*>(item_facts_iface))) {
        g_sdk = nullptr;
        g_world_action_port = nullptr;
        g_world_facts = nullptr;
        g_item_facts = nullptr;
        g_install_error = "install failed: item facts unavailable";
        return 1;
    }
    g_item_facts = static_cast<const contracts::ItemFactsApi*>(item_facts_iface);
    return 0;
}

void plugin_deactivate(contracts::PluginContext& ctx) {
    (void)ctx;
    g_installed = false;
    g_backup_call_map_name = nullptr;
    g_world_facts = nullptr;
    g_item_facts = nullptr;
    g_world_action_port = nullptr;
    g_sdk = nullptr;
    invalidate_operations();
}

void plugin_on_enabled(contracts::PluginContext& ctx) {
    (void)ctx;
    g_installed = true;
    g_install_error = nullptr;
}

void plugin_on_disabled(contracts::PluginContext& ctx) {
    (void)ctx;
    g_installed = false;
    invalidate_operations();
}

void plugin_fill_status(contracts::PluginStatusOut& out) {
    out.enabled = g_installed;
    out.error = g_install_error;
}

int32_t plugin_on_resource_ready(contracts::PluginContext& ctx, const char* resource_id,
                                 void* backup) {
    (void)ctx;
    if (resource_id == nullptr || std::strcmp(resource_id, "hook:UIPlay_CallMapName") != 0 ||
        backup == nullptr) {
        return 1;
    }
    g_backup_call_map_name = reinterpret_cast<uint64_t (*)()>(backup);
    return 0;
}

// Feature callback does not replace the existing lifecycle or business controls.
constexpr contracts::FeatureDefinition kFeatures[] = {
    {"world_teleport.map_jump", 1, contracts::FeatureCategory::kBasicOptimization,
     contracts::FeatureType::kToggle, "更好的地图传送对话框", "在地图边界传送点提供地图跳转选择与费用确认。",
     [](contracts::PluginContext&, uint8_t) -> QolResult { return contracts::ok(); }, nullptr},
};

const contracts::PluginDescriptor kDescriptor = {
    kPluginId,
    kPluginVersion,
    2,
    kRequires,
    1,
    kOptionalRequires,
    0,
    nullptr,
    1,
    kResources,
    0,
    nullptr,
    &plugin_install,
    &plugin_deactivate,
    &plugin_on_enabled,
    &plugin_on_disabled,
    &plugin_fill_status,
    &plugin_on_resource_ready,
    kPorts,
    1,
    nullptr,
    0,
    kFeatures, 1,
};

}  // namespace

const contracts::PluginDescriptor& plugin_descriptor() {
    return kDescriptor;
}

const contracts::QolHostSdk* host_sdk() {
    return g_sdk;
}

const contracts::QolWorldActionPort* world_action_port() {
    return g_world_action_port;
}


const contracts::WorldFactsApi* world_facts() {
    return g_world_facts;
}

const contracts::ItemFactsApi* item_facts() {
    return g_item_facts;
}

}  // namespace qol::features::world_teleport
