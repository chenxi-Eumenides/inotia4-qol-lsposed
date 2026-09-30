#include "plugin.h"

#include "../../contracts/ui_choice.h"
#include "../../contracts/game_ports/choice_port.h"
#include "../../contracts/game_ports/ports.h"
#include "../../contracts/http.h"
#include "feature.h"

#include <cstdlib>
#include <cstring>
#include <string>

namespace qol::features::world_teleport {
namespace {

constexpr const char* kPluginId = "world_teleport";
constexpr const char* kPluginVersion = "0.1.0";

const contracts::QolHostSdk* g_sdk = nullptr;
const contracts::QolGameHost* g_game = nullptr;
const contracts::QolChoicePort* g_choice_port = nullptr;
const contracts::WorldFactsApi* g_world_facts = nullptr;
const contracts::ItemFactsApi* g_item_facts = nullptr;
bool g_installed = false;
const char* g_install_error = nullptr;

// 入口为 UIPlay_CallMapName 尾部的声明式 kCallPatch。game/access/patches 负责两个版本
// 指纹与尾部回跳元数据，patch/ 负责近址 thunk、handler 调用、固定 mov x0,#1 回写和回滚。
const contracts::ResourceClaim kResources[] = {
    {
        "patch:UIPlay_CallMapName",
        "P_UI_PLAY_CALL_MAP_NAME",
        contracts::ResourceMechanism::kCallPatch,
        contracts::ResourcePolicy::kPrivate,
        contracts::ResourceRequirement::kRequired,
        contracts::ResourceFailure::kFailClosed,
        true,
        kPluginId,
        "世界态地图名入口重定向到传送选择框",
        nullptr,
        120,
        reinterpret_cast<void*>(&world_teleport_call_map_name),
    },
};

const contracts::CapabilityRequirement kOptionalRequires[] = {
    {contracts::kUiChoiceCapabilityId,
     QolVersionRange{static_cast<uint16_t>(contracts::kUiChoiceVersionMajor),
                     static_cast<uint16_t>(contracts::kUiChoiceVersionMinor)}},
};

const contracts::CapabilityRequirement kRequires[] = {
    {contracts::kWorldFactsCapabilityId,
     {contracts::kWorldFactsVersionMajor, contracts::kWorldFactsVersionMinor}},
    {contracts::kItemFactsCapabilityId,
     {contracts::kItemFactsVersionMajor, contracts::kItemFactsVersionMinor}},
};

const contracts::QolPortRequirement kPorts[] = {
    {contracts::QolPortId::QOL_PORT_CHOICE,
     {contracts::kQolPortChoiceVersion.major, contracts::kQolPortChoiceVersion.minor}},
};

int plugin_install(contracts::PluginContext& ctx) {
    g_sdk = ctx.sdk;
    g_game = ctx.game;
    g_choice_port = nullptr;
    g_world_facts = nullptr;
    g_item_facts = nullptr;
    g_installed = false;
    g_install_error = nullptr;
    if (ctx.struct_size < sizeof(contracts::PluginContext) ||
        ctx.sdk_major != contracts::kHostSdkMajor || g_sdk == nullptr || g_game == nullptr ||
        ctx.ports == nullptr || ctx.port_count != 1 || ctx.ports[0] == nullptr) {
        g_sdk = nullptr;
        g_game = nullptr;
        g_choice_port = nullptr;
        g_install_error = "install failed: world teleport dependencies not ready";
        return 1;
    }
    g_choice_port = static_cast<const contracts::QolChoicePort*>(ctx.ports[0]);
    const void* world_facts_iface = nullptr;
    if (g_sdk->capabilities.require == nullptr ||
        !contracts::succeeded(g_sdk->capabilities.require(
            contracts::kWorldFactsCapabilityId,
            {contracts::kWorldFactsVersionMajor, contracts::kWorldFactsVersionMinor},
            &world_facts_iface)) ||
        world_facts_iface == nullptr) {
        g_sdk = nullptr;
        g_game = nullptr;
        g_choice_port = nullptr;
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
        g_game = nullptr;
        g_choice_port = nullptr;
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
    g_world_facts = nullptr;
    g_item_facts = nullptr;
    g_game = nullptr;
    g_choice_port = nullptr;
    g_sdk = nullptr;
}

void plugin_on_enabled(contracts::PluginContext& ctx) {
    (void)ctx;
    g_installed = true;
    g_install_error = nullptr;
}

void plugin_on_disabled(contracts::PluginContext& ctx) {
    (void)ctx;
    g_installed = false;
}

void plugin_fill_status(contracts::PluginStatusOut& out) {
    out.enabled = g_installed;
    out.error = g_install_error;
}

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
    nullptr,
    kPorts,
    1,
    nullptr,
    0,
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

const contracts::QolChoicePort* choice_port() {
    return g_choice_port;
}

const contracts::WorldFactsApi* world_facts() {
    return g_world_facts;
}

const contracts::ItemFactsApi* item_facts() {
    return g_item_facts;
}

}  // namespace qol::features::world_teleport
