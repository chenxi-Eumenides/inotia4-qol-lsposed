#include "plugin.h"

#include <cstdint>

#include "../../contracts/world_tuning.h"
#include "feature.h"

namespace qol::features::better_move {
namespace {

constexpr const char* kPluginId = "better_move";
constexpr const char* kPluginVersion = "0.1.0";

const contracts::QolPortRequirement kPorts[] = {
    {contracts::QolPortId::QOL_PORT_MOVEMENT_INPUT,
     {contracts::kQolPortMovementInputVersion.major,
      contracts::kQolPortMovementInputVersion.minor}},
};

// 移速能力由 world_tuning 提供；缺失时移动逻辑回退原生 8px，插件仍可独立使用。
const contracts::CapabilityRequirement kOptionalRequires[] = {
    {contracts::kWorldTuningMoveSpeedCapabilityId,
     {contracts::kWorldTuningMoveSpeedVersion.major,
      contracts::kWorldTuningMoveSpeedVersion.minor}},
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
        "方向触摸唯一入口；捕获按下/拖动/抬起并写入自定义移动键码",
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
        "游戏内按键接收器；把自定义模式键码翻译成原版主方向键码",
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
        "玩家方向键分支的 bl CHAR_Move；按选择模式的主/副轴向量移动",
        nullptr,
        120,
        reinterpret_cast<void*>(&char_move_wrapper),
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
        "UIPlayPorting_Draw 原函数返回后按 selected 落点向量绘制旋转指针",
        nullptr,
        120,
        reinterpret_cast<void*>(&porting_draw_wrapper),
    },
    {
        kResourceIdWaitCoord,
        "F_CHAR_MOVE_ON_WAIT_COORD_VMA",
        contracts::ResourceMechanism::kEntryHook,
        contracts::ResourcePolicy::kPrivate,
        contracts::ResourceRequirement::kRequired,
        contracts::ResourceFailure::kFailClosed,
        true,
        kPluginId,
        "主控角色无路径时取消 8px 补步；其他角色/路径保留原版",
        nullptr,
        120,
        reinterpret_cast<void*>(&move_on_wait_coord_wrapper),
    },
    {
        kResourceIdMapBlocking,
        "F_MAP_IS_BLOCKING_BYPIXEL_VMA",
        contracts::ResourceMechanism::kEntryHook,
        contracts::ResourcePolicy::kPrivate,
        contracts::ResourceRequirement::kRequired,
        contracts::ResourceFailure::kFailClosed,
        true,
        kPluginId,
        "对齐坐标透传；非对齐坐标按完整 16px 碰撞盒查询瓦片",
        nullptr,
        120,
        reinterpret_cast<void*>(&map_blocking_wrapper),
    },
    {
        kResourceIdReleaseAlign,
        "P_GAMESTATE_PRESSKEY_RELEASE_ALIGN",
        contracts::ResourceMechanism::kCallPatch,
        contracts::ResourcePolicy::kPrivate,
        contracts::ResourceRequirement::kRequired,
        contracts::ResourceFailure::kFailClosed,
        true,
        kPluginId,
        "松手未对齐分支改为正常停止动作路径",
        nullptr,
        120,
        nullptr,
    },
    {
        kResourceIdSetActionAlign,
        "P_CHAR_SET_ACTION_ALIGN",
        contracts::ResourceMechanism::kCallPatch,
        contracts::ResourcePolicy::kPrivate,
        contracts::ResourceRequirement::kRequired,
        contracts::ResourceFailure::kFailClosed,
        true,
        kPluginId,
        "CHAR_SetAction 未对齐分支改为继续动作切换",
        nullptr,
        120,
        nullptr,
    },
};

constexpr contracts::FeatureChoiceOption kMovementModes[] = {
    {"eight_way", "八向移动"},
    {"omnidirectional", "全向移动"},
};

constexpr contracts::FeatureDefinition kFeatures[] = {
    {"better_move.direction", 0, contracts::FeatureCategory::kGameplay,
     contracts::FeatureType::kChoice, "移动优化", "选择关闭、八向或全向移动。",
     nullptr, nullptr, nullptr, 0, kMovementModes,
     static_cast<uint32_t>(sizeof(kMovementModes) / sizeof(kMovementModes[0])), &on_choice},
};

int plugin_install(contracts::PluginContext& ctx) {
    remove_poll_task();
    set_gate(false);
    set_input_port(nullptr);
    reset_move_speed();
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
        port->api.reset == nullptr || port->api.handle_wait_coord == nullptr ||
        port->api.map_blocking_query == nullptr || port->api.prepare_overlay == nullptr) {
        return 1;
    }
    if (!contracts::movement_input_port_supports_mode(port) ||
        !contracts::movement_input_port_supports_step(port)) {
        return 1;
    }
    set_host_sdk(ctx.sdk);
    set_input_port(port);
    set_gate(true);
    ensure_poll_task();
    ensure_move_speed_subscription(true);
    return 0;
}

void plugin_deactivate(contracts::PluginContext& ctx) {
    (void)ctx;
    remove_poll_task();
    set_gate(false);
    const auto* port = input_port();
    if (port != nullptr && port->api.set_mode != nullptr)
        port->api.set_mode(contracts::QolMovementInputMode::kDisabled);
    reset_move_speed();
    set_host_sdk(nullptr);
}

void plugin_on_enabled(contracts::PluginContext& ctx) {
    (void)ctx;
    set_gate(true);
    ensure_poll_task();
    ensure_move_speed_subscription(true);
}

void plugin_on_disabled(contracts::PluginContext& ctx) {
    (void)ctx;
    remove_poll_task();
    set_gate(false);
    const auto* port = input_port();
    if (port != nullptr && port->api.set_mode != nullptr)
        port->api.set_mode(contracts::QolMovementInputMode::kDisabled);
    reset_move_speed();
}

void plugin_fill_status(contracts::PluginStatusOut& out) {
    out.enabled = gate_enabled();
    out.error = nullptr;
}

const contracts::PluginDescriptor kDescriptor = {
    kPluginId,
    kPluginVersion,
    0, nullptr,   // requires
    static_cast<uint32_t>(sizeof(kOptionalRequires) / sizeof(kOptionalRequires[0])),
    kOptionalRequires,  // optional: world_tuning.move_speed
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

const contracts::PluginDescriptor& plugin_descriptor() { return kDescriptor; }

}  // namespace qol::features::better_move
