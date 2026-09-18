#include "plugins/stack_limit/plugin.h"

#include <cstdint>

#include "contracts/stack_limit.h"

// 堆叠上限：6 条 clamp 指令 patch 的声明式所有者。原实现（feature/patch/game_patch.cpp
// 的 g_stack_enabled + PatchSet）在 B2a-2 拆除，有效态改由 core/native/stack_limit_state
// 持有，物理安装/还原交宿主按 gate 同步。
//
// 门禁：descriptor.resources 的 6 条 gate 全为 "plugin.stack_limit.enabled"；宿主
// install_all/sync_gates 据此安装/还原，并调用本插件的 on_enabled/on_disabled 更新有效态。

namespace qol::features::stack_limit {
namespace {

constexpr const char* kPluginId = "stack_limit";
constexpr const char* kPluginVersion = "0.1.0";
constexpr const char* kGate = "plugin.stack_limit.enabled";
constexpr const char* kReason =
    "数量上限 clamp：判定点比较数为完整数量值（getter 全量/入参总量），999 clamp 只允许"
    "作用于这些点；直接读 b 段残量的点不进资源表";

#define STACK_LIMIT_CLAIM(NAME)                                                        \
    {NAME, NAME, contracts::ResourceMechanism::kCallPatch,                             \
     contracts::ResourcePolicy::kPrivate, contracts::ResourceRequirement::kRequired,   \
     contracts::ResourceFailure::kFailClosed, true, kPluginId, kReason, kGate, 120,    \
     nullptr}

const contracts::ResourceClaim kResources[] = {
    STACK_LIMIT_CLAIM("P_INVEN_MOVE_ITEM_CLAMP_150"),
    STACK_LIMIT_CLAIM("P_INVEN_MOVE_ITEM_CLAMP_158"),
    STACK_LIMIT_CLAIM("P_INVEN_SAVE_ITEM_DIRECT_CLAMP_DC"),
    STACK_LIMIT_CLAIM("P_INVEN_SAVE_ITEM_DIRECT_CLAMP_F0"),
    STACK_LIMIT_CLAIM("P_INVEN_SAVE_ITEM_DATA_CLAMP_7C"),
    STACK_LIMIT_CLAIM("P_INVEN_FIND_SAVE_SLOT_CLAMP_238"),
};
#undef STACK_LIMIT_CLAIM

// 自身初始化：无资源外动作（clamp 由宿主按 gate 安装）。幂等。
int plugin_install(contracts::PluginContext& ctx) {
    (void)ctx;
    return 0;
}

// deactivate：资源随还原事务物理归还，有效态必须同步置 false——否则读侧按 S2 全量而
// 写侧 clamp 已缺位，数量语义不一致。门禁关闭时 on_disabled 同样置 false（幂等）。
void plugin_deactivate(contracts::PluginContext& ctx) {
    (void)ctx;
    stack_limit_set_effective(false);
}

void plugin_on_enabled(contracts::PluginContext& ctx) {
    (void)ctx;
    stack_limit_set_effective(true);
}

void plugin_on_disabled(contracts::PluginContext& ctx) {
    (void)ctx;
    stack_limit_set_effective(false);
}

void plugin_fill_status(contracts::PluginStatusOut& out) {
    out.enabled = stack_limit_enabled();
    out.error = nullptr;
}

const contracts::PluginDescriptor kDescriptor = {
    kPluginId,
    kPluginVersion,
    0, nullptr,   // requires
    0, nullptr,   // optional_requires
    0, nullptr,   // provides
    6, kResources,
    0, nullptr,   // jni（开关经命令端口下发）
    &plugin_install,
    &plugin_deactivate,
    &plugin_on_enabled,
    &plugin_on_disabled,
    &plugin_fill_status,
    nullptr,      // on_resource_ready（非入口机制，无 backup 交还）
};

}  // namespace

const contracts::PluginDescriptor& plugin_descriptor() {
    return kDescriptor;
}

}  // namespace qol::features::stack_limit
