#include "plugin.h"

#include "contracts/world_facts.h"

namespace qol::features::world_facts {
namespace {

constexpr const char* kPluginId = "world_facts";
constexpr const char* kPluginVersion = "0.1.0";

const contracts::QolHostSdk* g_sdk = nullptr;
const contracts::QolGameHost* g_game = nullptr;

QolResult not_ready() {
    return contracts::error(kQolErrNotReady, "world facts not ready", 21);
}

QolResult state(int32_t* out_state) {
    if (g_game == nullptr || g_game->read_only.world.state == nullptr) return not_ready();
    return g_game->read_only.world.state(out_state);
}

QolResult game_state_gate(int32_t* out_open) {
    if (g_game == nullptr || g_game->read_only.world.game_state_gate == nullptr) return not_ready();
    return g_game->read_only.world.game_state_gate(out_open);
}

QolResult map_record_count(int32_t* out_count) {
    if (g_game == nullptr || g_game->read_only.world.map_record_count == nullptr) return not_ready();
    return g_game->read_only.world.map_record_count(out_count);
}

QolResult current_map_id(int32_t* out_map_id) {
    if (g_game == nullptr || g_game->read_only.world.current_map_id == nullptr) return not_ready();
    return g_game->read_only.world.current_map_id(out_map_id);
}

QolResult map_name(int32_t map_id, const char** out_name) {
    if (g_game == nullptr || g_game->read_only.world.map_name == nullptr) return not_ready();
    return g_game->read_only.world.map_name(map_id, out_name);
}

QolResult active_player_direction(int32_t* out_direction) {
    if (g_game == nullptr || g_game->read_only.world.active_player_direction == nullptr) {
        return not_ready();
    }
    return g_game->read_only.world.active_player_direction(out_direction);
}

QolResult money(int64_t* out_money) {
    if (g_game == nullptr || g_game->read_only.world.money == nullptr) return not_ready();
    return g_game->read_only.world.money(out_money);
}

QolResult tutorial_state(int32_t* out_state) {
    if (g_game == nullptr || g_game->read_only.world.tutorial_state == nullptr) return not_ready();
    return g_game->read_only.world.tutorial_state(out_state);
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

void log_message(const contracts::QolHostSdk* sdk, const char* message) {
    if (sdk == nullptr || sdk->log.write == nullptr || message == nullptr) return;
    uint32_t length = 0;
    while (message[length] != '\0') ++length;
    (void)sdk->log.write(1, 1, kPluginId, message, length);
}

int plugin_install(contracts::PluginContext& ctx) {
    g_sdk = ctx.sdk;
    g_game = ctx.game;
    if (ctx.struct_size < sizeof(contracts::PluginContext) ||
        ctx.sdk_major != contracts::kHostSdkMajor || g_sdk == nullptr || g_game == nullptr) {
        g_sdk = nullptr;
        g_game = nullptr;
        return 1;
    }
    log_message(g_sdk, "[world_facts] provider install");
    return 0;
}

void plugin_deactivate(contracts::PluginContext& ctx) {
    log_message(ctx.sdk, "[world_facts] provider deactivate");
    g_game = nullptr;
    g_sdk = nullptr;
}

void plugin_on_enabled(contracts::PluginContext& ctx) {
    log_message(ctx.sdk, "[world_facts] provider enabled: capability published");
}

void plugin_on_disabled(contracts::PluginContext& ctx) {
    log_message(ctx.sdk, "[world_facts] provider disabled: capability withdrawn");
}

void plugin_fill_status(contracts::PluginStatusOut& out) {
    out.enabled = g_game != nullptr;
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
};

}  // namespace

const contracts::PluginDescriptor& plugin_descriptor() {
    return kDescriptor;
}

}  // namespace qol::features::world_facts
