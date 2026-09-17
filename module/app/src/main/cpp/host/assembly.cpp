#include "host/assembly.h"

#include <cstdint>

#include "core/native/call_patch.h"
#include "core/native/qol_log.h"
#include "feature/patch/native_inventory_hook.h"
#include "features/push_all/plugin.h"
#include "features/quest/plugin.h"
#include "features/simple_mode/plugin.h"
#include "game/access/patches.h"
#include "game/access/symbols.h"
#include "game/clock.h"
#include "hooks/transaction.h"
#include "host/frame_source.h"
#include "host/plugin_registry.h"
#include "infra/commands.h"
#include "infra/config.h"
#include "infra/runtime.h"
#include "infra/scheduler.h"
#include "patch/instr_patch.h"

namespace qol::host {
namespace {

bool g_assembly_done = false;

using MechanismOutcome = qol::hooks::TxDeps::MechanismOutcome;

// 资源门禁读取：事务引擎回调，gate 为空视为开。
bool gate_on(const char* key) {
    return key == nullptr ? true : qol::infra::config_get_bool(key, false);
}

// 非入口机制的物理安装/释放分派（hooks/ 不依赖 patch/，机制归宿主注入）。
// call patch 的站点由 game/access/patches 解析后交给 patch/instr_patch。
MechanismOutcome dispatch_install(const contracts::ResourceClaim& claim, uintptr_t target) {
    (void)target;  // call patch 的站点由本层解析，引擎无需预解析
    if (claim.mechanism != contracts::ResourceMechanism::kCallPatch) {
        return MechanismOutcome{1, "mechanism unsupported"};
    }
    qol::game::PatchSite site{};
    if (!qol::game::patch_site(claim.symbol, &site)) {
        return MechanismOutcome{1, "site not found"};
    }
    const qol::patch::Outcome outcome = qol::patch::install_claim(claim, site.addr);
    return MechanismOutcome{outcome.ok, outcome.error};
}

MechanismOutcome dispatch_release(const contracts::ResourceClaim& claim, uintptr_t target) {
    (void)target;
    if (claim.mechanism != contracts::ResourceMechanism::kCallPatch) {
        return MechanismOutcome{1, "mechanism unsupported"};
    }
    qol::game::PatchSite site{};
    if (!qol::game::patch_site(claim.symbol, &site)) {
        return MechanismOutcome{1, "site not found"};
    }
    const qol::patch::Outcome outcome = qol::patch::release_claim(claim, site.addr);
    return MechanismOutcome{outcome.ok, outcome.error};
}

// 逻辑帧任务：命令排空 → 生命周期推进 → 门禁对齐（主线程）。
// 经 infra::scheduler 逻辑帧点派发；帧源 wrapper 同时派发 legacy frame_task，
// 未迁移消费者（autosell/quest/save_enter/transition_dispatch/char_next_exp_cache 等）
// 在各自迁移前继续由 legacy 管理器工作。
// 顺序原因：先执行命令（可能触发 lifecycle_apply 启动事务），再推进生命周期事务，
// 最后门禁对齐；sync_gates 只处理 kEnabled/kInstalledDisabled，不会与事务中间态冲突。
// 返回 true = 保留期任务，不自动注销。
bool drain_commands_task(int64_t frame, void* ctx) {
    (void)frame;
    (void)ctx;
    infra::commands_drain();
    lifecycle_step();
    sync_gates();
    return true;
}

}  // namespace

bool assembly_init() {
    // 仅在完整成功后置位：失败可重试，成功则幂等。
    if (g_assembly_done) return true;

    // 帧时钟注入：infra 不依赖 game，游戏事实由 host 装配时提供（见 infra/runtime.h）。
    infra::runtime_init(infra::Clock{&qol::game::frame_count});

    // 帧源常驻资源：渲染/逻辑锚点。未就绪时 fail-closed 返回 false；不阻断插件注册，
    // 后续可重试（gamebridge 在 bridge 就绪后亦会先调用一次）。
    if (!frame_source_install()) {
        QOL_LOG_WARN(QolDomain::kCore, "assembly: frame source not ready");
    }

    // 命令通道每逻辑帧排空一次；命令体在主线程执行（§6.1）。
    infra::scheduler_add(infra::FramePoint::kLogicPre, &drain_commands_task, nullptr, 1, 0);

    // hook/unhook 原语过渡期取自 feature/patch；P5 迁往 patch/ 后改引 patch 层。
    // native_hook_func/native_unhook_func 是机制原语的获取器（返回函数指针）；这里先取获取器
    // 地址再调用：本处只是把机制原语注入宿主，不构成某个游戏符号的安装点（真实安装点在
    // 插件描述符声明、由插件注册表执行），直接写调用式会被 check_resources 按文本
    // `native_hook_func(` 识别成未登记资源安装，而基线棘轮只允许收缩。
    NativeHookFunType (*hook_getter)() = native_hook_func;
    NativeUnhookFunType (*unhook_getter)() = native_unhook_func;
    const qol::hooks::NativeHookFn hook_fn = hook_getter();
    const qol::hooks::NativeUnhookFn unhook_fn = unhook_getter();
    const HostDeps deps{
        &qol::game::resolve_hook_entry,
        hook_fn,
        unhook_fn,
    };

    // 指令 patch 原语注入：过渡期指向 core/native/call_patch；P5 原语迁入 patch/ 后本处改指 patch 层。
    patch::set_primitives(qol::patch::Primitives{
        &call_patch_install_bl,
        &call_patch_revert_bl,
        &call_patch_install_word,
        &call_patch_revert_word,
    });

    // 事务引擎依赖（运行期生命周期路径）与生命周期命令执行入口。
    hooks::tx_set_deps(qol::hooks::TxDeps{
        &qol::game::resolve_hook_entry,
        hook_fn,
        unhook_fn,
        &gate_on,
        &dispatch_install,
        &dispatch_release,
    });
    infra::commands_set_lifecycle_handler(&lifecycle_apply);

    if (!register_plugin(features::simple_mode::plugin_descriptor())) {
        QOL_LOG_ERROR(QolDomain::kCore, "assembly: simple_mode registration failed");
        return false;
    }
    if (!register_plugin(features::push_all::plugin_descriptor())) {
        QOL_LOG_ERROR(QolDomain::kCore, "assembly: push_all registration failed");
        return false;
    }
    if (!register_plugin(features::quest::plugin_descriptor())) {
        QOL_LOG_ERROR(QolDomain::kCore, "assembly: quest registration failed");
        return false;
    }

    // 启动期同步安装：此刻尚无帧源与在途回调，直接装完即就绪；运行期生命周期一律走事务。
    const int failed = install_all(deps);
    if (failed != 0) {
        QOL_LOG_WARN(QolDomain::kCore, "assembly: %d plugin(s) failed to install", failed);
        return false;
    }
    g_assembly_done = true;
    QOL_LOG_INFO(QolDomain::kCore, "assembly: all plugins ready");
    return true;
}

}  // namespace qol::host
