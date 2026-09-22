#include "plugin.h"

#include "contracts/save_facts.h"
#include "contracts/game_ports/save_port.h"

namespace qol::features::save_facts {
namespace {

constexpr const char* kPluginId = "save_facts";
constexpr const char* kPluginVersion = "0.1.0";
[[gnu::used]] constexpr char kSaveFactsBuildMarker[] = "save.facts:v1";

const contracts::QolSavePort* g_port = nullptr;

QolResult not_ready() {
    return contracts::error(kQolErrNotReady, "save facts not ready", 20);
}

QolResult slot_state(int32_t slot, int32_t* out_state) {
    if (g_port == nullptr || g_port->read.slot_state == nullptr) return not_ready();
    return g_port->read.slot_state(slot, out_state);
}
QolResult slot_exists(int32_t slot, int32_t* out_exists) {
    if (g_port == nullptr || g_port->read.slot_exists == nullptr) return not_ready();
    return g_port->read.slot_exists(slot, out_exists);
}
QolResult slot_hero_index(int32_t slot, int32_t* out_index) {
    if (g_port == nullptr || g_port->read.slot_hero_index == nullptr) return not_ready();
    return g_port->read.slot_hero_index(slot, out_index);
}
QolResult hero_level(int32_t slot, int32_t* out_level) {
    if (g_port == nullptr || g_port->read.hero_level == nullptr) return not_ready();
    return g_port->read.hero_level(slot, out_level);
}
QolResult hero_class(int32_t slot, int32_t* out_class) {
    if (g_port == nullptr || g_port->read.hero_class == nullptr) return not_ready();
    return g_port->read.hero_class(slot, out_class);
}
QolResult load_data(int32_t slot, void** out_data, int32_t* out_size) {
    if (g_port == nullptr || g_port->read.load_data == nullptr) return not_ready();
    return g_port->read.load_data(slot, out_data, out_size);
}
QolResult hub_key(const char** out_key) {
    if (g_port == nullptr || g_port->read.hub_key == nullptr) return not_ready();
    return g_port->read.hub_key(out_key);
}
QolResult map_id(int32_t slot, int32_t* out_map_id) {
    if (g_port == nullptr || g_port->read.map_id == nullptr) return not_ready();
    return g_port->read.map_id(slot, out_map_id);
}
QolResult current_slot(int32_t* out_slot) {
    if (g_port == nullptr || g_port->read.current_slot == nullptr) return not_ready();
    return g_port->read.current_slot(out_slot);
}
QolResult is_ok(int32_t* out_ok) {
    if (g_port == nullptr || g_port->read.is_ok == nullptr) return not_ready();
    return g_port->read.is_ok(out_ok);
}

constexpr contracts::SaveFactsApi kSaveFactsApi{
    &slot_state, &slot_exists, &slot_hero_index, &hero_level, &hero_class,
    &load_data, &hub_key, &map_id, &current_slot, &is_ok,
};

const contracts::CapabilityProvisionRef kProvides[] = {
    {contracts::kSaveFactsCapabilityId, contracts::kSaveFactsVersion, &kSaveFactsApi},
};

const contracts::QolPortRequirement kPorts[] = {
    {contracts::QolPortId::QOL_PORT_SAVE,
     {contracts::kQolPortSaveVersion.major, contracts::kQolPortSaveVersion.minor}},
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
    g_port = static_cast<const contracts::QolSavePort*>(ctx.ports[0]);
    if (!contracts::satisfies(g_port->abi_version,
                              {contracts::kQolPortSaveVersion.major,
                               contracts::kQolPortSaveVersion.minor}) ||
        g_port->read.slot_state == nullptr || g_port->read.load_data == nullptr ||
        g_port->read.is_ok == nullptr) {
        g_port = nullptr;
        return 1;
    }
    log_message(ctx.sdk, "[save_facts] provider install");
    return 0;
}

void plugin_deactivate(contracts::PluginContext& ctx) {
    log_message(ctx.sdk, "[save_facts] provider deactivate");
    g_port = nullptr;
}

void plugin_on_enabled(contracts::PluginContext& ctx) {
    log_message(ctx.sdk, "[save_facts] provider enabled: capability published");
}

void plugin_on_disabled(contracts::PluginContext& ctx) {
    log_message(ctx.sdk, "[save_facts] provider disabled: capability withdrawn");
}

void plugin_fill_status(contracts::PluginStatusOut& out) {
    out.enabled = g_port != nullptr;
    out.error = nullptr;
}

const contracts::PluginDescriptor kDescriptor = {
    kPluginId, kPluginVersion,
    0, nullptr,
    0, nullptr,
    1, kProvides,
    0, nullptr,
    0, nullptr,
    &plugin_install, &plugin_deactivate, &plugin_on_enabled, &plugin_on_disabled,
    &plugin_fill_status, nullptr,
    kPorts, 1,
};

}  // namespace

const contracts::PluginDescriptor& plugin_descriptor() { return kDescriptor; }

}  // namespace qol::features::save_facts
