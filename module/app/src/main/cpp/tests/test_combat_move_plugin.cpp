#include "contracts/game_ports/combat_move_port.h"
#include "contracts/plugin.h"
#include "plugins/combat_move/plugin.h"

#include <cassert>
#include <cstdint>
#include <cstring>

namespace {

void* g_target = reinterpret_cast<void*>(0x10);
int8_t g_character_type = 0;
int g_default_attack = 1;
int g_current_action_present = 1;
int g_action_marker = 0;
int32_t g_walk = 1;
int32_t g_distance = 20;
int32_t g_range = 5;
int32_t g_no_move = 1;
int32_t g_set_target_calls = 0;
int32_t g_stop_calls = 0;
int32_t g_set_action_backup_calls = 0;
int32_t g_start_action_backup_calls = 0;
void* g_forwarded_target = nullptr;

int32_t active_player(void*) { return 1; }
int32_t player_side(void*) { return g_character_type == 0 ? 1 : 0; }
void* target(void*) { return g_target; }
const void* current_action(void*) {
    return g_current_action_present != 0 ? &g_action_marker : nullptr;
}
int32_t is_default_attack(void*, const void* action) {
    return action != nullptr && g_default_attack != 0;
}
int32_t is_walk_action(void*, const void*) { return g_walk; }
int32_t distance(void*, void*) { return g_distance; }
int32_t action_range(void*, const void*) { return g_range; }
int32_t is_no_move(void*) { return g_no_move; }
void set_target(void*, void* value) {
    ++g_set_target_calls;
    g_target = value;
}
void stop_combat(void*) { ++g_stop_calls; }

void set_action_backup(void*, void*, void* target_value) {
    ++g_set_action_backup_calls;
    g_forwarded_target = target_value;
}
int32_t start_action_backup(void*, void*, int32_t) {
    ++g_start_action_backup_calls;
    return 37;
}

}  // namespace

int main() {
    using namespace qol;
    const contracts::PluginDescriptor& descriptor =
        features::combat_move::plugin_descriptor();
    assert(std::strcmp(descriptor.id, "combat_move") == 0);
    assert(std::strcmp(descriptor.version, "0.1.0") == 0);
    assert(descriptor.resources_count == 4);
    assert(descriptor.port_count == 1);
    assert(descriptor.ports[0].id == contracts::QolPortId::QOL_PORT_COMBAT_MOVE);
    for (uint32_t i = 0; i < descriptor.resources_count; ++i) {
        const contracts::ResourceClaim& resource = descriptor.resources[i];
        assert(resource.resumable);
        assert(resource.atomic);
        assert(resource.requirement == contracts::ResourceRequirement::kRequired);
        assert(resource.failure == contracts::ResourceFailure::kFailClosed);
    }
    assert(descriptor.resources[0].mechanism == contracts::ResourceMechanism::kCallPatch);
    assert(descriptor.resources[1].mechanism == contracts::ResourceMechanism::kCallPatch);
    assert(descriptor.resources[2].mechanism == contracts::ResourceMechanism::kEntryHook);
    assert(descriptor.resources[3].mechanism == contracts::ResourceMechanism::kEntryHook);
    assert(descriptor.feature_count == 1);
    assert(descriptor.features[0].default_enabled == 0);
    assert(descriptor.features[0].category == contracts::FeatureCategory::kGameplay);

    const contracts::QolCombatMovePort port{
        contracts::kQolPortCombatMoveVersion, &active_player, &player_side, &target,
        &current_action, &is_default_attack, &is_walk_action, &distance, &action_range,
        &is_no_move, &set_target, &stop_combat};
    const void* ports[] = {&port};
    contracts::QolHostSdk sdk{};
    contracts::QolGameHost game{};
    contracts::PluginContext ctx{sizeof(contracts::PluginContext),
                                 contracts::kHostSdkMajor, 0, &sdk, &game,
                                 "combat_move", ports, 1, 0};
    assert(descriptor.install(ctx) == 0);
    descriptor.on_enabled(ctx);
    assert(features::combat_move::on_resource_ready(
               ctx, "hook:CHAR_SetAction", reinterpret_cast<void*>(&set_action_backup)) == 0);
    assert(features::combat_move::on_resource_ready(
               ctx, "hook:CHAR_StartActionEx", reinterpret_cast<void*>(&start_action_backup)) == 0);
    assert(features::combat_move::on_resource_ready(
               ctx, "patch:GAMESTATE_PressKeyPlay+0x1d4", nullptr) == 0);
    assert(features::combat_move::on_resource_ready(ctx, "patch:CHAR_Move+0x40", nullptr) == 0);
    assert(features::combat_move::on_resource_ready(ctx, "hook:unknown", nullptr) == 0);
    assert(features::combat_move::on_resource_ready(ctx, "hook:CHAR_SetAction", nullptr) == 0);

    features::combat_move::set_action_wrapper(reinterpret_cast<void*>(0x1),
                                                reinterpret_cast<void*>(0x2),
                                                reinterpret_cast<void*>(0x3));
    assert(g_set_action_backup_calls == 0);
    g_character_type = 1;
    features::combat_move::set_action_wrapper(reinterpret_cast<void*>(0x1),
                                                reinterpret_cast<void*>(0x2),
                                                reinterpret_cast<void*>(0x3));
    assert(g_set_action_backup_calls == 1);
    assert(g_forwarded_target == reinterpret_cast<void*>(0x3));
    assert(features::combat_move::move_gate_wrapper(reinterpret_cast<void*>(0x1)) == g_no_move);
    g_distance = 20;
    g_target = reinterpret_cast<void*>(0x10);
    assert(features::combat_move::start_action_ex_wrapper(
               reinterpret_cast<void*>(0x1), reinterpret_cast<void*>(0x2), 0) == 37);
    assert(g_target == reinterpret_cast<void*>(0x10));
    assert(g_stop_calls == 0);

    g_character_type = 0;
    g_default_attack = 0;
    assert(features::combat_move::start_action_ex_wrapper(
               reinterpret_cast<void*>(0x1), reinterpret_cast<void*>(0x2), 0) == 37);
    assert(g_target == reinterpret_cast<void*>(0x10));
    assert(g_stop_calls == 0);
    g_default_attack = 1;

    g_current_action_present = 1;
    assert(features::combat_move::move_gate_wrapper(reinterpret_cast<void*>(0x1)) == 0);
    g_set_target_calls = 0;
    features::combat_move::keep_target_wrapper(reinterpret_cast<void*>(0x1), nullptr);
    assert(g_set_target_calls == 0);

    g_distance = 20;
    assert(features::combat_move::start_action_ex_wrapper(
               reinterpret_cast<void*>(0x1), reinterpret_cast<void*>(0x2), 0) == 0);
    assert(g_target == nullptr);
    assert(g_stop_calls == 1);
    assert(g_start_action_backup_calls == 2);

    g_target = reinterpret_cast<void*>(0x10);
    g_distance = 4;
    assert(features::combat_move::start_action_ex_wrapper(
               reinterpret_cast<void*>(0x1), reinterpret_cast<void*>(0x2), 0) == 37);
    assert(g_start_action_backup_calls == 3);
    descriptor.deactivate(ctx);
    assert(features::combat_move::move_gate_wrapper(reinterpret_cast<void*>(0x1)) == 1);
}
