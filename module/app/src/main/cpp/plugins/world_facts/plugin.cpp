#include "plugin.h"

#include "contracts/world_facts.h"
#include "contracts/game_ports/world_port.h"

namespace qol::features::world_facts {
namespace {

constexpr const char* kPluginId = "world_facts";
constexpr const char* kPluginVersion = "0.1.0";

const contracts::QolHostSdk* g_sdk = nullptr;
const contracts::QolWorldPort* g_port = nullptr;

QolResult not_ready() {
    return contracts::error(kQolErrNotReady, "world facts not ready", 21);
}

QolResult state(int32_t* out_state) {
    if (g_port == nullptr || g_port->read.state == nullptr) return not_ready();
    return g_port->read.state(out_state);
}

QolResult game_state_gate(int32_t* out_open) {
    if (g_port == nullptr || g_port->read.game_state_gate == nullptr) return not_ready();
    return g_port->read.game_state_gate(out_open);
}

QolResult map_record_count(int32_t* out_count) {
    if (g_port == nullptr || g_port->read.map_record_count == nullptr) return not_ready();
    return g_port->read.map_record_count(out_count);
}

QolResult current_map_id(int32_t* out_map_id) {
    if (g_port == nullptr || g_port->read.current_map_id == nullptr) return not_ready();
    return g_port->read.current_map_id(out_map_id);
}

QolResult map_name(int32_t map_id, const char** out_name) {
    if (g_port == nullptr || g_port->read.map_name == nullptr) return not_ready();
    return g_port->read.map_name(map_id, out_name);
}

QolResult active_player_direction(int32_t* out_direction) {
    if (g_port == nullptr || g_port->read.active_player_direction == nullptr) {
        return not_ready();
    }
    return g_port->read.active_player_direction(out_direction);
}

QolResult money(int64_t* out_money) {
    if (g_port == nullptr || g_port->read.money == nullptr) return not_ready();
    return g_port->read.money(out_money);
}

QolResult tutorial_state(int32_t* out_state) {
    if (g_port == nullptr || g_port->read.tutorial_state == nullptr) return not_ready();
    return g_port->read.tutorial_state(out_state);
}

constexpr contracts::WorldFactsApi kWorldFactsApi{
    &state,
    &game_state_gate,
    &map_record_count,
    &current_map_id,
    &map_name,
    &active_player_direction,
    &money,
    &tutorial_state,
};

const contracts::CapabilityProvisionRef kProvides[] = {
    {contracts::kWorldFactsCapabilityId, contracts::kWorldFactsVersion, &kWorldFactsApi},
};

const contracts::QolPortRequirement kPorts[] = {
    {contracts::QolPortId::QOL_PORT_WORLD, {1, 0}},
};

void log_message(const contracts::QolHostSdk* sdk, const char* message) {
    if (sdk == nullptr || sdk->log.write == nullptr || message == nullptr) return;
    uint32_t length = 0;
    while (message[length] != '\0') ++length;
    (void)sdk->log.write(1, 1, kPluginId, message, length);
}

int plugin_install(contracts::PluginContext& ctx) {
    g_sdk = ctx.sdk;
    g_port = nullptr;
    if (ctx.struct_size < contracts::PLUGIN_CONTEXT_PORTS_STRUCT_SIZE ||
        ctx.sdk_major != contracts::kHostSdkMajor || g_sdk == nullptr || ctx.ports == nullptr ||
        ctx.port_count != 1 || ctx.ports[0] == nullptr) {
        g_sdk = nullptr;
        return 1;
    }
    g_port = static_cast<const contracts::QolWorldPort*>(ctx.ports[0]);
    if (!contracts::satisfies(g_port->abi_version, {1, 0}) || g_port->read.state == nullptr ||
        g_port->read.game_state_gate == nullptr) {
        g_port = nullptr;
        g_sdk = nullptr;
        return 1;
    }
    log_message(g_sdk, "[world_facts] provider install");
    return 0;
}

void plugin_deactivate(contracts::PluginContext& ctx) {
    log_message(ctx.sdk, "[world_facts] provider deactivate");
    g_port = nullptr;
    g_sdk = nullptr;
}

void plugin_on_enabled(contracts::PluginContext& ctx) {
    log_message(ctx.sdk, "[world_facts] provider enabled: capability published");
}

void plugin_on_disabled(contracts::PluginContext& ctx) {
    log_message(ctx.sdk, "[world_facts] provider disabled: capability withdrawn");
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

}  // namespace qol::features::world_facts
