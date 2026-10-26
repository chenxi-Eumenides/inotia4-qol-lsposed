#include "plugin.h"

#include <cstdint>

#include "feature.h"

namespace qol::features::eight_way_move {
namespace {

constexpr const char* kPluginId = "eight_way_move";
constexpr const char* kPluginVersion = "0.1.0";

const contracts::QolPortRequirement kPorts[] = {
    {contracts::QolPortId::QOL_PORT_MOVEMENT_INPUT,
     {contracts::kQolPortMovementInputVersion.major,
      contracts::kQolPortMovementInputVersion.minor}},
};

// 全部资源跟随 lifecycle：feature 开启才安装，关闭即物理还原。
const contracts::ResourceClaim kResources[] = {
    {
        kResourceIdDirectionTouch,
        "F_UIPLAY_BUTTON_DIRECTION_VMA",
        contracts::ResourceMechanism::kEntryHook,
        contracts::ResourcePolicy::kPrivate,
        contracts::ResourceRequirement::kRequired,
        contracts::ResourceFailure::kFailClosed,
        true,
        kPluginId,
        "方向触摸唯一入口；捕获按下/拖动/抬起并写入自定义八向键码",
        nullptr,
        120,
        reinterpret_cast<void*>(&direction_touch_wrapper),
    },
    {
        kResourceIdPressKeyPlay,
        "F_GAMESTATE_PRESSKEYPLAY_VMA",
        contracts::ResourceMechanism::kEntryHook,
        contracts::ResourcePolicy::kPrivate,
        contracts::ResourceRequirement::kRequired,
        contracts::ResourceFailure::kFailClosed,
        true,
        kPluginId,
        "游戏内按键接收器；把自定义八向键码翻译成原版主方向键码",
        nullptr,
        120,
        reinterpret_cast<void*>(&press_key_play_wrapper),
    },
    {
        kResourceIdCharMove,
        "P_GAMESTATE_PRESSKEYPLAY_CHAR_MOVE",
        contracts::ResourceMechanism::kCallPatch,
        contracts::ResourcePolicy::kPrivate,
        contracts::ResourceRequirement::kRequired,
        contracts::ResourceFailure::kFailClosed,
        true,
        kPluginId,
        "玩家方向键分支的 bl CHAR_Move；斜向时追加副轴移动",
        nullptr,
        120,
        reinterpret_cast<void*>(&eight_way_char_move_wrapper),
    },
    {
        kResourceIdPortingDraw,
        "F_UI_PLAY_PORTING_DRAW_VMA",
        contracts::ResourceMechanism::kEntryHook,
        contracts::ResourcePolicy::kPrivate,
        contracts::ResourceRequirement::kRequired,
        contracts::ResourceFailure::kFailClosed,
        true,
        kPluginId,
        "UIPlayPorting_Draw 原函数返回后补画斜向第二根方向箭头",
        nullptr,
        120,
        reinterpret_cast<void*>(&porting_draw_wrapper),
    },
};

constexpr contracts::FeatureDefinition kFeatures[] = {
    {"eight_way_move.direction", 0, contracts::FeatureCategory::kGameplay,
     contracts::FeatureType::kToggle, "八向移动", "允许斜向移动",
     [](contracts::PluginContext&, uint8_t) -> QolResult { return contracts::ok(); }, nullptr},
};

int plugin_install(contracts::PluginContext& ctx) {
    remove_poll_task();
    set_gate(false);
    set_input_port(nullptr);
    set_host_sdk(nullptr);
    if (ctx.struct_size < sizeof(contracts::PluginContext) ||
        ctx.sdk_major != contracts::kHostSdkMajor || ctx.sdk == nullptr || ctx.game == nullptr ||
        ctx.ports == nullptr || ctx.port_count != 1 || ctx.ports[0] == nullptr) {
        return 1;
    }
    const auto* port = static_cast<const contracts::QolMovementInputPort*>(ctx.ports[0]);
    if (port->abi_version.major != contracts::kQolPortMovementInputVersion.major ||
        port->abi_version.minor < contracts::kQolPortMovementInputVersion.minor ||
        port->api.on_direction_touch == nullptr || port->api.translate_press_key == nullptr ||
        port->api.char_move == nullptr || port->api.set_enabled == nullptr ||
        port->api.reset == nullptr) {
        return 1;
    }
    set_host_sdk(ctx.sdk);
    set_input_port(port);
    set_gate(true);
    port->api.set_enabled(1);
    ensure_poll_task();
    return 0;
}

void plugin_deactivate(contracts::PluginContext& ctx) {
    (void)ctx;
    remove_poll_task();
    set_gate(false);
    const auto* port = input_port();
    if (port != nullptr && port->api.set_enabled != nullptr) port->api.set_enabled(0);
    set_host_sdk(nullptr);
}

void plugin_on_enabled(contracts::PluginContext& ctx) {
    (void)ctx;
    set_gate(true);
    const auto* port = input_port();
    if (port != nullptr && port->api.set_enabled != nullptr) port->api.set_enabled(1);
    ensure_poll_task();
}

void plugin_on_disabled(contracts::PluginContext& ctx) {
    (void)ctx;
    remove_poll_task();
    set_gate(false);
    const auto* port = input_port();
    if (port != nullptr && port->api.set_enabled != nullptr) port->api.set_enabled(0);
}

void plugin_fill_status(contracts::PluginStatusOut& out) {
    out.enabled = gate_enabled();
    out.error = nullptr;
}

const contracts::PluginDescriptor kDescriptor = {
    kPluginId,
    kPluginVersion,
    0, nullptr,   // requires
    0, nullptr,   // optional_requires
    0, nullptr,   // provides
    static_cast<uint32_t>(sizeof(kResources) / sizeof(kResources[0])), kResources,
    0, nullptr,   // jni
    &plugin_install,
    &plugin_deactivate,
    &plugin_on_enabled,
    &plugin_on_disabled,
    &plugin_fill_status,
    &on_resource_ready,
    kPorts,
    1,
    nullptr, 0,   // deprecated http_queries
    kFeatures, 1,
};

}  // namespace

const contracts::PluginDescriptor& plugin_descriptor() {
    return kDescriptor;
}

}  // namespace qol::features::eight_way_move
