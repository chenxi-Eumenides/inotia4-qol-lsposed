#include "plugin.h"

#include "contracts/character_facts.h"
#include "contracts/game_ports/character_port.h"

namespace qol::features::character_facts {
namespace {

constexpr const char* kPluginId = "character_facts";
constexpr const char* kPluginVersion = "0.1.0";

const contracts::QolCharacterPort* g_port = nullptr;

QolResult not_ready() {
    return contracts::error(kQolErrNotReady, "character facts not ready", 25);
}

QolResult type(const void* character, int32_t* out_type) {
    if (g_port == nullptr || g_port->read.type == nullptr) return not_ready();
    return g_port->read.type(character, out_type);
}

QolResult is_active_player_group(const void* character, int32_t* out_active) {
    if (g_port == nullptr || g_port->read.is_active_player_group == nullptr) {
        return not_ready();
    }
    return g_port->read.is_active_player_group(character, out_active);
}

constexpr contracts::CharacterFactsApi kCharacterFactsApi{
    &type,
    &is_active_player_group,
};

const contracts::CapabilityProvisionRef kProvides[] = {
    {contracts::kCharacterFactsCapabilityId, contracts::kCharacterFactsVersion, &kCharacterFactsApi},
};

const contracts::QolPortRequirement kPorts[] = {
    {contracts::QolPortId::QOL_PORT_CHARACTER, {1, 0}},
};

void log_message(const contracts::QolHostSdk* sdk, const char* message) {
    if (sdk == nullptr || sdk->log.write == nullptr || message == nullptr) return;
    uint32_t length = 0;
    while (message[length] != '\0') ++length;
    (void)sdk->log.write(1, 1, kPluginId, message, length);
}

int plugin_install(contracts::PluginContext& ctx) {
    g_port = nullptr;
    if (ctx.struct_size < contracts::PLUGIN_CONTEXT_PORTS_STRUCT_SIZE ||
        ctx.sdk_major != contracts::kHostSdkMajor || ctx.sdk == nullptr || ctx.ports == nullptr ||
        ctx.port_count != 1 || ctx.ports[0] == nullptr) {
        return 1;
    }
    g_port = static_cast<const contracts::QolCharacterPort*>(ctx.ports[0]);
    if (!contracts::satisfies(g_port->abi_version, {1, 0}) || g_port->read.type == nullptr ||
        g_port->read.is_active_player_group == nullptr) {
        g_port = nullptr;
        return 1;
    }
    log_message(ctx.sdk, "[character_facts] provider install");
    return 0;
}

void plugin_deactivate(contracts::PluginContext& ctx) {
    log_message(ctx.sdk, "[character_facts] provider deactivate");
    g_port = nullptr;
}

void plugin_on_enabled(contracts::PluginContext& ctx) {
    log_message(ctx.sdk, "[character_facts] provider enabled: capability published");
}

void plugin_on_disabled(contracts::PluginContext& ctx) {
    log_message(ctx.sdk, "[character_facts] provider disabled: capability withdrawn");
}

void plugin_fill_status(contracts::PluginStatusOut& out) {
    out.enabled = g_port != nullptr;
    out.error = nullptr;
}

const contracts::PluginDescriptor kDescriptor = {
    kPluginId,
    kPluginVersion,
    0,
    nullptr,
    0,
    nullptr,
    1,
    kProvides,
    0,
    nullptr,
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
};

}  // namespace

const contracts::PluginDescriptor& plugin_descriptor() {
    return kDescriptor;
}

}  // namespace qol::features::character_facts
