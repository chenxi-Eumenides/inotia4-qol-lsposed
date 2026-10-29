#include "plugins/better_move/plugin.h"
#include "plugins/better_move/feature.h"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace {

int failures = 0;
int mode_calls = 0;
qol::contracts::QolMovementInputMode last_mode =
    qol::contracts::QolMovementInputMode::kDisabled;

void legacy_set_enabled(uint8_t) {}
void set_mode(qol::contracts::QolMovementInputMode mode) {
    ++mode_calls;
    last_mode = mode;
}
void set_step(int32_t) {}

void check(bool condition, const char* what) {
    if (condition) return;
    std::printf("FAIL: %s\n", what);
    ++failures;
}

}  // namespace

int main() {
    using qol::contracts::FeatureType;
    using qol::contracts::QolMovementInputMode;
    static_assert(offsetof(qol::contracts::QolMovementInputApi, set_enabled) == 40);
    static_assert(offsetof(qol::contracts::QolMovementInputApi, set_mode) == 80);
    static_assert(offsetof(qol::contracts::QolMovementInputApi, set_step) == 88);
    static_assert(sizeof(qol::contracts::QolMovementInputApi) == 96);

    const auto& d = qol::features::better_move::plugin_descriptor();
    check(std::strcmp(d.id, "better_move") == 0, "plugin id");
    check(d.resources_count == 8, "resource count");
    check(d.port_count == 1, "port count");
    check(d.port_count == 1 &&
              d.ports[0].id == qol::contracts::QolPortId::QOL_PORT_MOVEMENT_INPUT,
          "movement input port");
    check(d.ports[0].range.min_minor == 3, "movement input port minor 3");
    check(d.feature_count == 1, "feature count");
    check(std::strcmp(d.features[0].id, "better_move.direction") == 0, "feature id");
    check(std::strcmp(d.features[0].name, "移动优化") == 0, "feature name");
    check(std::strcmp(d.features[0].description, "选择关闭、八向或全向移动。") == 0,
          "feature description unchanged");
    check(d.optional_count == 1 && d.optional_requires != nullptr &&
              std::strcmp(d.optional_requires[0].id, "world_tuning.move_speed") == 0,
          "world tuning move speed is an optional dependency");
    check(d.features[0].default_enabled == 0, "feature default off");
    check(d.features[0].type == FeatureType::kChoice, "feature uses choice mode");
    check(d.features[0].choices != nullptr && d.features[0].choice_count == 2,
          "feature exposes two stable modes");
    check(std::strcmp(d.features[0].choices[0].value, "eight_way") == 0 &&
              std::strcmp(d.features[0].choices[1].value, "omnidirectional") == 0,
          "feature stable mode values");
    check(d.features[0].on_choice != nullptr && d.features[0].on_toggle == nullptr &&
              d.features[0].on_entry == nullptr,
          "choice uses only on_choice callback");
    check(d.features[0].category == qol::contracts::FeatureCategory::kGameplay,
          "feature in gameplay category");

    qol::contracts::QolMovementInputPort old_port{{1, 1}, {}};
    old_port.api.set_enabled = &legacy_set_enabled;
    check(!qol::contracts::movement_input_port_supports_mode(&old_port),
          "v1.1 port remains prefix-only for new mode consumer");
    qol::contracts::QolMovementInputPort v12_port{{1, 2}, {}};
    v12_port.api.set_enabled = &legacy_set_enabled;
    v12_port.api.set_mode = &set_mode;
    check(qol::contracts::movement_input_port_supports_mode(&v12_port) &&
              !qol::contracts::movement_input_port_supports_step(&v12_port),
          "v1.2 port has mode but no step tail");
    qol::contracts::QolMovementInputPort mode_port{
        qol::contracts::kQolPortMovementInputVersion, {}};
    mode_port.api.set_enabled = &legacy_set_enabled;
    mode_port.api.set_mode = &set_mode;
    mode_port.api.set_step = &set_step;
    check(qol::contracts::movement_input_port_supports_mode(&mode_port),
          "v1.2 mode tail is available");
    check(qol::contracts::movement_input_port_supports_step(&mode_port),
          "v1.3 step tail is available");
    check(qol::contracts::legacy_movement_input_mode(1) ==
              QolMovementInputMode::kOmnidirectional &&
              qol::contracts::legacy_movement_input_mode(0) ==
              QolMovementInputMode::kDisabled,
          "legacy set_enabled mapping remains full-mode compatible");
    qol::features::better_move::set_input_port(&mode_port);
    qol::contracts::PluginContext context{};
    const auto& choice = d.features[0];
    check(qol::contracts::succeeded(choice.on_choice(context, nullptr)) &&
              last_mode == QolMovementInputMode::kDisabled,
          "null choice closes movement mode");
    check(qol::contracts::succeeded(choice.on_choice(context, "eight_way")) &&
              last_mode == QolMovementInputMode::kEightWay,
          "eight_way choice reaches mode setter");
    check(qol::contracts::succeeded(choice.on_choice(context, "omnidirectional")) &&
              last_mode == QolMovementInputMode::kOmnidirectional,
          "omnidirectional choice reaches mode setter");
    const int calls_before_invalid = mode_calls;
    check(choice.on_choice(context, "invalid").code == kQolErrInvalidArgument &&
              mode_calls == calls_before_invalid,
          "invalid choice rejected without changing mode");
    qol::features::better_move::set_input_port(nullptr);

    check(std::strcmp(d.resources[0].id, "hook:UIPlay_ButtonDirection") == 0, "direction id");
    check(d.resources[0].mechanism == qol::contracts::ResourceMechanism::kEntryHook,
          "direction entry hook");
    check(std::strcmp(d.resources[1].id, "hook:GAMESTATE_PressKeyPlay") == 0, "press key id");
    check(d.resources[1].mechanism == qol::contracts::ResourceMechanism::kEntryHook,
          "press key entry hook");
    check(std::strcmp(d.resources[2].id, "patch:GAMESTATE_PressKeyPlay+0x21c") == 0,
          "char move id");
    check(d.resources[2].mechanism == qol::contracts::ResourceMechanism::kCallPatch,
          "char move call patch");
    check(std::strcmp(d.resources[3].id, "hook:UIPlayPorting_Draw") == 0, "porting draw id");
    check(d.resources[3].mechanism == qol::contracts::ResourceMechanism::kEntryHook,
          "porting draw entry hook");
    check(std::strcmp(d.resources[4].id, "hook:CHAR_MoveOnWaitCoord") == 0, "wait coord id");
    check(d.resources[4].mechanism == qol::contracts::ResourceMechanism::kEntryHook,
          "wait coord entry hook");
    check(std::strcmp(d.resources[5].id, "hook:MAP_IsBlockingByPixel") == 0,
          "map blocking id");
    check(d.resources[5].mechanism == qol::contracts::ResourceMechanism::kEntryHook,
          "map blocking entry hook");
    check(std::strcmp(d.resources[6].id, "patch:GAMESTATE_PressKey+0x1c0") == 0,
          "release align id");
    check(d.resources[6].mechanism == qol::contracts::ResourceMechanism::kCallPatch,
          "release word replace");
    check(std::strcmp(d.resources[7].id, "patch:CHAR_SetAction+0x26c") == 0,
          "set action align id");
    check(d.resources[7].mechanism == qol::contracts::ResourceMechanism::kCallPatch,
          "set action word replace");

    for (uint32_t i = 0; i < d.resources_count; ++i) {
        check(d.resources[i].policy == qol::contracts::ResourcePolicy::kPrivate, "private");
        check(d.resources[i].requirement == qol::contracts::ResourceRequirement::kRequired,
              "required");
        check(d.resources[i].failure == qol::contracts::ResourceFailure::kFailClosed,
              "fail closed");
        if (i >= 6) {
            check(d.resources[i].handler == nullptr, "word patch has no handler");
        } else {
            check(d.resources[i].handler != nullptr, "handler");
        }
    }

    if (failures != 0) {
        std::printf("%d failure(s)\n", failures);
        return 1;
    }
    std::printf("better_move_plugin: ok\n");
    return 0;
}
