#include "plugins/inventory_hooks/plugin.h"

#include <cstdint>

// 过渡依赖（B2a-1 → B2c）：库存 wrapper 的 claims 表与 backup 目标仍留在
// feature/patch/native_inventory_hook.cpp（handler 只能在该 TU 取址），本插件经两个
// 自由函数桥接。两个方向都不 include 对方头文件：include 会形成 feature→feature 层
// 依赖（架构违规），与 plugins/special_equip/plugin.cpp 的前置声明先例一致。
// 清除阶段：B2c 把 wrapper 业务搬入本插件后，两函数随业务迁入并删除前置声明。
const qol::contracts::ResourceClaim* inventory_hooks_claims(uint32_t* out_count);
int32_t inventory_hooks_bind_backup(const char* resource_id, void* backup);

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

int plugin_install(contracts::PluginContext& ctx) {
    (void)ctx;
    // 自身初始化无额外动作：资源由宿主声明式安装，wrapper 业务体仍在 feature/patch。
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
    // 原函数入口交还 wrapper 所有者；桥对未知 id / 空 backup 返回非 0 → 宿主逆序回滚本
    // 插件全部资源并置 failed（fail-closed，禁止 wrapper 在 backup 缺失下运行）。
    return inventory_hooks_bind_backup(resource_id, backup);
}

}  // namespace

const contracts::PluginDescriptor& plugin_descriptor() {
    // claims 表本体在 feature/patch（handler 取址限制），此处首次调用经桥取得 24 条声明；
    // 函数内静态保证描述符生命周期 ≥ 进程。
    static const contracts::PluginDescriptor descriptor = [] {
        uint32_t count = 0;
        const contracts::ResourceClaim* claims = inventory_hooks_claims(&count);
        return contracts::PluginDescriptor{
            kPluginId,
            kPluginVersion,
            0, nullptr,   // requires
            0, nullptr,   // optional_requires
            0, nullptr,   // provides
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
