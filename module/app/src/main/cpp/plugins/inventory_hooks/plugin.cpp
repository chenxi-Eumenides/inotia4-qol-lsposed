#include "plugins/inventory_hooks/plugin.h"
#include "plugins/inventory_hooks/claims.h"
#include "plugins/inventory_hooks/native_inventory_hook.h"

#include <cstdint>

#include "contracts/inventory_refresh.h"

// D2c-2a：native_inventory_hook / inventory_hook_stage4 已迁入本插件目录（原
// feature/patch）。claims 表与 backup 交还入口同层，经 claims.h 直接引用；原为避免
// feature→feature 层依赖而设的跨层全局桥前置声明已删除。
//
// D2c-2b-1：经 `inventory_refresh` capability 暴露 raw-original refresh trampoline，
// 供 extension_bag / game_patch 等消费者现取；出售门禁改由 hooks/dispatch 布尔门承载
// （谓词注册方 special_equip），本插件不再导出 set_sell_gate。
//
// B2a-1 禁止 deactivate：extension_bag / 自动出售 / 合成等 legacy 消费者直接依赖这 24 个
// hook 的 wrapper 语义（扩展袋读取、扩展补扣、出售门禁等）。deactivate 会由宿主物理
// 释放全部资源，令 legacy 消费者在 wrapper 缺失下运行；本阶段只允许进程内常驻。
// 清除条件：消费者改经 capability requires 收口（B2c/D2）后解除，见
// docs/refactor/map.md 过渡依赖区。宿主生命周期端点没有"不可 deactivate"标志，本约束
// 靠流程与文档保证，不在代码内拦截。

namespace qol::features::inventory_hooks {
namespace {

constexpr const char* kPluginId = "inventory_hooks";
constexpr const char* kPluginVersion = "0.1.0";

// 能力接口实例：函数地址编译期常量；宿主在 enabled 时注册、revoking 时注销。
constexpr contracts::InventoryRefreshApi kInventoryRefreshApi{
    &inventory_native_hook_call_refresh_item_area_original,
};

const contracts::CapabilityProvisionRef kProvides[] = {
    {
        contracts::kInventoryRefreshCapabilityId,
        {contracts::kInventoryRefreshVersionMajor, contracts::kInventoryRefreshVersionMinor},
        &kInventoryRefreshApi,
    },
};

int plugin_install(contracts::PluginContext& ctx) {
    (void)ctx;
    // 自身初始化无额外动作：资源由宿主声明式安装，wrapper 业务体在本插件 TU。
    return 0;
}

// 空操作幂等：资源物理释放由宿主逆序执行，本插件无额外状态需要清理。
void plugin_deactivate(contracts::PluginContext& ctx) {
    (void)ctx;
}

void plugin_on_enabled(contracts::PluginContext& ctx) {
    (void)ctx;
}

void plugin_on_disabled(contracts::PluginContext& ctx) {
    (void)ctx;
}

// 常驻无门禁：状态恒为 enabled。
void plugin_fill_status(contracts::PluginStatusOut& out) {
    out.enabled = true;
    out.error = nullptr;
}

int32_t on_resource_ready(contracts::PluginContext& ctx, const char* resource_id, void* backup) {
    (void)ctx;
    // 原函数入口交还 wrapper 所有者；同目录 claims.h 入口对未知 id / 空 backup 返回非 0
    // → 宿主逆序回滚本插件全部资源并置 failed（fail-closed，禁止 wrapper 在 backup 缺失下运行）。
    return inventory_hooks_bind_backup(resource_id, backup);
}

}  // namespace

const contracts::PluginDescriptor& plugin_descriptor() {
    // claims 表本体在 native_inventory_hook.cpp（handler 取址限制），本插件同层直接引用；
    // 函数内静态保证描述符生命周期 ≥ 进程。
    static const contracts::PluginDescriptor descriptor = [] {
        uint32_t count = 0;
        const contracts::ResourceClaim* claims = inventory_hooks_claims(&count);
        return contracts::PluginDescriptor{
            kPluginId,
            kPluginVersion,
            0, nullptr,   // requires
            0, nullptr,   // optional_requires
            1, kProvides,  // provides（inventory_refresh v1.0）
            count, claims,
            0, nullptr,   // jni
            &plugin_install,
            &plugin_deactivate,
            &plugin_on_enabled,
            &plugin_on_disabled,
            &plugin_fill_status,
            &on_resource_ready,
        };
    }();
    return descriptor;
}

}  // namespace qol::features::inventory_hooks
