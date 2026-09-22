#include "plugin.h"

#include <cstdint>
#include <cstring>

#include "feature.h"

namespace qol::features::quest {
namespace {

constexpr const char* kPluginId = "quest";
constexpr const char* kPluginVersion = "0.1.0";
constexpr uint32_t kSetStateHandlerId = 0x51535401;

struct SetStatePayload {
    uint32_t quest_id;
    int32_t state;
};
static_assert(sizeof(SetStatePayload) <= contracts::kTypedCommandPayloadCapacity,
              "quest typed command payload exceeds capacity");

const contracts::QolHostSdk* g_sdk = nullptr;
const contracts::QolGameHost* g_game = nullptr;
const contracts::WorldFactsApi* g_world_facts = nullptr;
const contracts::QolQuestPort* g_quest_port = nullptr;

const contracts::CapabilityRequirement kRequires[] = {
    {contracts::kWorldFactsCapabilityId,
     {contracts::kWorldFactsVersionMajor, contracts::kWorldFactsVersionMinor}},
};

const contracts::QolPortRequirement kPorts[] = {
    {contracts::QolPortId::QOL_PORT_QUEST, {1, 0}},
};

bool set_state_handler(void*, const uint8_t* payload, uint32_t size) {
    if (g_quest_port == nullptr || g_quest_port->write.set_state == nullptr ||
        payload == nullptr || size != sizeof(SetStatePayload)) {
        return false;
    }
    SetStatePayload command{};
    std::memcpy(&command, payload, sizeof(command));
    return g_quest_port->write.set_state(command.quest_id, command.state).ok == kQolOk;
}

int plugin_install(contracts::PluginContext& ctx) {
    g_sdk = ctx.sdk;
    g_game = ctx.game;
    g_world_facts = nullptr;
    g_quest_port = nullptr;
    if (ctx.struct_size < contracts::PLUGIN_CONTEXT_PORTS_STRUCT_SIZE ||
        ctx.sdk_major != contracts::kHostSdkMajor || g_sdk == nullptr || g_game == nullptr) {
        g_sdk = nullptr;
        g_game = nullptr;
        return 1;
    }
    if (ctx.ports == nullptr || ctx.port_count != 1 || ctx.ports[0] == nullptr) {
        g_sdk = nullptr;
        g_game = nullptr;
        return 1;
    }
    g_quest_port = static_cast<const contracts::QolQuestPort*>(ctx.ports[0]);
    if (!contracts::satisfies(g_quest_port->abi_version, {1, 0}) ||
        g_quest_port->read.active_slot_count == nullptr ||
        g_quest_port->write.set_state == nullptr ||
        g_sdk->commands.register_typed_handler == nullptr ||
        g_sdk->commands.register_typed_handler(kSetStateHandlerId, &set_state_handler, nullptr).ok !=
            kQolOk) {
        g_quest_port = nullptr;
        g_sdk = nullptr;
        g_game = nullptr;
        return 1;
    }
    const void* world_facts_iface = nullptr;
    if (g_sdk->capabilities.require == nullptr ||
        !contracts::succeeded(g_sdk->capabilities.require(
            contracts::kWorldFactsCapabilityId,
            {contracts::kWorldFactsVersionMajor, contracts::kWorldFactsVersionMinor},
            &world_facts_iface)) ||
        world_facts_iface == nullptr) {
        (void)g_sdk->commands.unregister_typed_handler(kSetStateHandlerId);
        g_sdk = nullptr;
        g_game = nullptr;
        g_quest_port = nullptr;
        return 1;
    }
    g_world_facts = static_cast<const contracts::WorldFactsApi*>(world_facts_iface);
    set_gate(true);
    start_resync_task();
    return 0;
}

void plugin_deactivate(contracts::PluginContext& ctx) {
    (void)ctx;
    if (g_sdk != nullptr && g_sdk->commands.unregister_typed_handler != nullptr) {
        (void)g_sdk->commands.unregister_typed_handler(kSetStateHandlerId);
    }
    set_gate(false);
    cancel_resync_task();
    g_world_facts = nullptr;
    g_quest_port = nullptr;
    g_game = nullptr;
    g_sdk = nullptr;
}

// 零资源、无门禁键：启停只切业务门禁，不做资源操作（§3.3）。
void plugin_on_enabled(contracts::PluginContext& ctx) {
    (void)ctx;
    set_gate(true);
    start_resync_task();
}

void plugin_on_disabled(contracts::PluginContext& ctx) {
    (void)ctx;
    set_gate(false);
    cancel_resync_task();
}

void plugin_fill_status(contracts::PluginStatusOut& out) {
    out.enabled = gate_enabled();
    out.error = nullptr;
}

const contracts::PluginDescriptor kDescriptor = {
    kPluginId,
    kPluginVersion,
    1, kRequires,  // requires=world.facts
    0, nullptr,   // optional_requires
    0, nullptr,   // provides
    0, nullptr,   // resources（零资源）
    0, nullptr,   // jni
    &plugin_install,
    &plugin_deactivate,
    &plugin_on_enabled,
    &plugin_on_disabled,
    &plugin_fill_status,
    nullptr,      // on_resource_ready（无资源）
    kPorts,
    1,
};

}  // namespace

const contracts::QolHostSdk* host_sdk() { return g_sdk; }

const contracts::QolGameHost* game_host() { return g_game; }

const contracts::WorldFactsApi* world_facts() { return g_world_facts; }

const contracts::QolQuestPort* quest_port() { return g_quest_port; }

bool post_set_state(uint32_t quest_id, int32_t state) {
    if (g_sdk == nullptr || g_sdk->commands.post_typed == nullptr) return false;
    const SetStatePayload payload{quest_id, state};
    contracts::QolCommandTicket ticket = 0;
    return g_sdk->commands.post_typed(kSetStateHandlerId, &payload, sizeof(payload), &ticket).ok ==
           kQolOk;
}

const contracts::PluginDescriptor& plugin_descriptor() {
    return kDescriptor;
}

}  // namespace qol::features::quest
