#include "plugin.h"

#include "contracts/character_facts.h"

namespace qol::features::character_facts {
namespace {

constexpr const char* kPluginId = "character_facts";
constexpr const char* kPluginVersion = "0.1.0";

const contracts::QolGameHost* g_game = nullptr;

QolResult not_ready() {
    return contracts::error(kQolErrNotReady, "character facts not ready", 25);
}

QolResult type(const void* character, int32_t* out_type) {
    if (g_game == nullptr || g_game->read.character.type == nullptr) return not_ready();
    return g_game->read.character.type(character, out_type);
}

QolResult is_active_player_group(const void* character, int32_t* out_active) {
    if (g_game == nullptr || g_game->read.character.is_active_player_group == nullptr) {
        return not_ready();
    }
    return g_game->read.character.is_active_player_group(character, out_active);
}

constexpr contracts::CharacterFactsApi kCharacterFactsApi{
    &type,
    &is_active_player_group,
};

const contracts::CapabilityProvisionRef kProvides[] = {
    {contracts::kCharacterFactsCapabilityId, contracts::kCharacterFactsVersion, &kCharacterFactsApi},
};

void log_message(const contracts::QolHostSdk* sdk, const char* message) {
    if (sdk == nullptr || sdk->log.write == nullptr || message == nullptr) return;
    uint32_t length = 0;
    while (message[length] != '\0') ++length;
    (void)sdk->log.write(1, 1, kPluginId, message, length);
}

int plugin_install(contracts::PluginContext& ctx) {
    if (ctx.struct_size < sizeof(contracts::PluginContext) ||
        ctx.sdk_major != contracts::kHostSdkMajor || ctx.sdk == nullptr || ctx.game == nullptr) {
        g_game = nullptr;
        return 1;
    }
    g_game = ctx.game;
    log_message(ctx.sdk, "[character_facts] provider install");
    return 0;
}

void plugin_deactivate(contracts::PluginContext& ctx) {
    log_message(ctx.sdk, "[character_facts] provider deactivate");
    g_game = nullptr;
}

void plugin_on_enabled(contracts::PluginContext& ctx) {
    log_message(ctx.sdk, "[character_facts] provider enabled: capability published");
}

void plugin_on_disabled(contracts::PluginContext& ctx) {
    log_message(ctx.sdk, "[character_facts] provider disabled: capability withdrawn");
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

}  // namespace qol::features::character_facts
