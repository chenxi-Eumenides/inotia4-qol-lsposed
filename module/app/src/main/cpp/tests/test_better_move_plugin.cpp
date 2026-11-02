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
int step_calls = 0;
int32_t last_step = -1;

void legacy_set_enabled(uint8_t) {}
void set_mode(qol::contracts::QolMovementInputMode mode) {
    ++mode_calls;
    last_mode = mode;
}
void set_step(int32_t step) {
    ++step_calls;
    last_step = step;
}

// --- mock Host feature choice API（覆盖空 selection 自动回写默认 "8" 的路径）---
int read_choice_calls = 0;
int set_choice_calls = 0;
bool mock_has_selection = false;
qol::contracts::QolOwnerToken seeded_token = 0;
char seeded_plugin[32] = {0};
char seeded_key[32] = {0};
char seeded_value[8] = {0};

QolResult dummy_namespace(void*, qol::contracts::QolOwnerToken, const char*, char*, uint32_t,
                          uint32_t*) {
    return qol::contracts::error(kQolErrUnsupported, nullptr, 0);
}
QolResult dummy_read_feature(void*, qol::contracts::QolOwnerToken, const char*, const char*,
                             uint8_t*) {
    return qol::contracts::error(kQolErrUnsupported, nullptr, 0);
}
QolResult dummy_set_feature(void*, qol::contracts::QolOwnerToken, const char*, const char*, uint8_t,
                            qol::contracts::FeatureReceipt*) {
    return qol::contracts::error(kQolErrUnsupported, nullptr, 0);
}
QolResult dummy_read_config(void*, qol::contracts::QolOwnerToken, const char*, const char*, char*,
                            uint32_t, uint32_t*) {
    return qol::contracts::error(kQolErrUnsupported, nullptr, 0);
}
QolResult dummy_set_config(void*, qol::contracts::QolOwnerToken, const char*, const char*,
                           const char*, uint32_t, qol::contracts::FeatureReceipt*) {
    return qol::contracts::error(kQolErrUnsupported, nullptr, 0);
}
QolResult dummy_receipt(void*, qol::contracts::QolOwnerToken, uint64_t,
                        qol::contracts::FeatureReceipt*) {
    return qol::contracts::error(kQolErrUnsupported, nullptr, 0);
}
QolResult dummy_read_catalog(void*, qol::contracts::QolOwnerToken, char*, uint32_t, uint32_t*) {
    return qol::contracts::error(kQolErrUnsupported, nullptr, 0);
}
QolResult dummy_read_desired(void*, qol::contracts::QolOwnerToken, const char*, uint8_t*, uint8_t*) {
    return qol::contracts::error(kQolErrUnsupported, nullptr, 0);
}
QolResult dummy_request_desired(void*, qol::contracts::QolOwnerToken, const char*, uint8_t,
                                qol::contracts::FeatureReceipt*) {
    return qol::contracts::error(kQolErrUnsupported, nullptr, 0);
}
QolResult dummy_invoke_entry(void*, qol::contracts::QolOwnerToken, const char*, const char*) {
    return qol::contracts::error(kQolErrUnsupported, nullptr, 0);
}
QolResult mock_read_choice(void*, qol::contracts::QolOwnerToken, const char*, const char*, char* out,
                           uint32_t capacity, uint32_t* size) {
    ++read_choice_calls;
    if (!mock_has_selection) {
        *size = 0;
        return qol::contracts::ok();
    }
    if (capacity < 2) return qol::contracts::error(kQolErrInvalidArgument, nullptr, 0);
    out[0] = '1';
    out[1] = '2';
    *size = 2;
    return qol::contracts::ok();
}
QolResult mock_set_choice(void*, qol::contracts::QolOwnerToken token, const char* plugin_id,
                          const char* key, const char* value, qol::contracts::FeatureReceipt*) {
    ++set_choice_calls;
    seeded_token = token;
    std::snprintf(seeded_plugin, sizeof(seeded_plugin), "%s", plugin_id);
    std::snprintf(seeded_key, sizeof(seeded_key), "%s", key);
    std::snprintf(seeded_value, sizeof(seeded_value), "%s", value);
    return qol::contracts::ok();
}

qol::contracts::QolFeatureApi make_mock_feature_api() {
    qol::contracts::QolFeatureApi api{};
    api.struct_size = sizeof(qol::contracts::QolFeatureApi);
    api.read_namespace = &dummy_namespace;
    api.read_feature = &dummy_read_feature;
    api.request_set_feature = &dummy_set_feature;
    api.read_config = &dummy_read_config;
    api.request_set_config = &dummy_set_config;
    api.receipt = &dummy_receipt;
    api.read_catalog = &dummy_read_catalog;
    api.read_desired = &dummy_read_desired;
    api.request_desired = &dummy_request_desired;
    api.invoke_entry = &dummy_invoke_entry;
    api.read_choice = &mock_read_choice;
    api.request_set_choice = &mock_set_choice;
    return api;
}

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
    check(d.feature_count == 2, "feature count");
    check(std::strcmp(d.features[0].id, "better_move.direction") == 0, "feature id");
    check(std::strcmp(d.features[0].name, "移动优化") == 0, "feature name");
    check(std::strcmp(d.features[0].description, "选择关闭、八向或全向移动。") == 0,
          "feature description unchanged");
    check(d.optional_count == 0 && d.optional_requires == nullptr,
          "no optional capability: step is a local choice now");
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

    const auto& step = d.features[1];
    check(std::strcmp(step.id, "better_move.step") == 0, "step feature id");
    check(std::strcmp(step.name, "移动步长") == 0, "step feature name");
    check(std::strcmp(step.description, "每步像素数，8 为原生；仅八向/全向移动时生效。") == 0,
          "step feature description");
    check(step.default_enabled == 0, "step default off");
    check(step.type == FeatureType::kChoice, "step uses choice mode");
    check(step.category == qol::contracts::FeatureCategory::kGameplay,
          "step in gameplay category");
    check(step.choices != nullptr && step.choice_count == 8, "step exposes eight choices");
    const char* expected_steps[] = {"1", "3", "6", "8", "10", "12", "16", "24"};
    for (uint32_t i = 0; i < 8; ++i) {
        check(std::strcmp(step.choices[i].value, expected_steps[i]) == 0 &&
                  std::strcmp(step.choices[i].label, expected_steps[i]) == 0,
              "step choice value and label match");
    }
    check(step.on_choice != nullptr && step.on_toggle == nullptr && step.on_entry == nullptr,
          "step uses only on_choice callback");

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

    // --- on_step_choice：8 个合法档位推送到端口 set_step ---
    const int32_t expected_amounts[] = {1, 3, 6, 8, 10, 12, 16, 24};
    for (uint32_t i = 0; i < 8; ++i) {
        step_calls = 0;
        check(qol::contracts::succeeded(step.on_choice(context, expected_steps[i])),
              "step value accepted");
        check(step_calls == 1 && last_step == expected_amounts[i],
              "step value pushed to port set_step");
    }
    step_calls = 0;
    check(step.on_choice(context, "5").code == kQolErrInvalidArgument, "step 5 rejected");
    check(step.on_choice(context, "0").code == kQolErrInvalidArgument, "step 0 rejected");
    check(step.on_choice(context, "40").code == kQolErrInvalidArgument, "step 40 rejected");
    check(step_calls == 0, "rejected step values never push");
    // 上一档为 24：空字符串/null 都表示关闭态，回退原生 8 并推送。
    check(qol::contracts::succeeded(step.on_choice(context, "")),
          "empty string is the closed state");
    check(step_calls == 1 && last_step == 8, "empty string falls back to native 8 and pushes");
    check(qol::contracts::succeeded(step.on_choice(context, "12")), "step 12 selected again");
    step_calls = 0;
    check(qol::contracts::succeeded(step.on_choice(context, nullptr)), "null step accepted");
    check(step_calls == 1 && last_step == 8, "null step falls back to native 8 and pushes");

    // --- 停用路径：reset_step 强制回原生 8 ---
    check(qol::contracts::succeeded(step.on_choice(context, "12")), "step 12 for deactivate test");
    step_calls = 0;
    qol::features::better_move::reset_step();
    check(step_calls == 1 && last_step == 8, "reset_step forces native 8 through the port");

    // --- 空 selection 自动回写默认 "8"（mock Host choice API）---
    qol::contracts::QolFeatureApi mock_api = make_mock_feature_api();
    qol::contracts::QolHostSdk mock_sdk{};
    mock_sdk.abi = {sizeof(qol::contracts::QolHostSdk), qol::contracts::kHostSdkMajor,
                    qol::contracts::kHostSdkMinor};
    mock_sdk.feature = &mock_api;
    qol::features::better_move::set_gate(true);
    context.sdk = &mock_sdk;
    context.owner_token = 0xABCD;

    read_choice_calls = 0;
    set_choice_calls = 0;
    mock_has_selection = false;
    check(qol::contracts::succeeded(step.on_choice(context, nullptr)), "empty step with api");
    check(read_choice_calls == 1, "empty step reads stored selection");
    check(set_choice_calls == 1 && seeded_token == 0xABCD &&
              std::strcmp(seeded_plugin, "better_move") == 0 &&
              std::strcmp(seeded_key, "better_move.step") == 0 &&
              std::strcmp(seeded_value, "8") == 0,
          "missing selection seeds default 8 via request_set_choice");

    read_choice_calls = 0;
    set_choice_calls = 0;
    mock_has_selection = true;
    check(qol::contracts::succeeded(step.on_choice(context, nullptr)),
          "empty step with existing selection");
    check(read_choice_calls == 1 && set_choice_calls == 0,
          "existing selection is never overwritten");

    read_choice_calls = 0;
    set_choice_calls = 0;
    qol::features::better_move::set_gate(false);
    check(qol::contracts::succeeded(step.on_choice(context, nullptr)),
          "empty step while plugin inactive");
    check(read_choice_calls == 0 && set_choice_calls == 0,
          "inactive plugin does not touch selection");

    qol::features::better_move::set_gate(true);
    read_choice_calls = 0;
    set_choice_calls = 0;
    mock_has_selection = false;
    context.sdk = nullptr;
    check(qol::contracts::succeeded(step.on_choice(context, nullptr)),
          "empty step without host sdk stays local");
    check(set_choice_calls == 0, "missing api keeps local default without crashing");
    qol::features::better_move::set_gate(false);

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
