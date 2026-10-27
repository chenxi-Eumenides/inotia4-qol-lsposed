#include "plugins/eight_way_move/plugin.h"

#include <cstdint>
#include <cstdio>
#include <cstring>

namespace {

int failures = 0;

void check(bool condition, const char* what) {
    if (condition) return;
    std::printf("FAIL: %s\n", what);
    ++failures;
}

}  // namespace

int main() {
    const auto& d = qol::features::eight_way_move::plugin_descriptor();
    check(std::strcmp(d.id, "eight_way_move") == 0, "plugin id");
    check(d.resources_count == 8, "resource count");
    check(d.port_count == 1, "port count");
    check(d.port_count == 1 &&
              d.ports[0].id == qol::contracts::QolPortId::QOL_PORT_MOVEMENT_INPUT,
          "movement input port");
    check(d.ports[0].range.min_minor == 1, "movement input port minor 1");
    check(d.feature_count == 1, "feature count");
    check(std::strcmp(d.features[0].id, "eight_way_move.direction") == 0, "feature id");
    check(d.features[0].default_enabled == 0, "feature default off");
    check(d.features[0].category == qol::contracts::FeatureCategory::kGameplay,
          "feature in gameplay category");

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
    std::printf("eight_way_move_plugin: ok\n");
    return 0;
}
