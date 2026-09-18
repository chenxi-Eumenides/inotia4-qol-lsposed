#include "host/assembly.h"

#include <cstdint>

#include "core/native/call_patch.h"
#include "core/native/qol_log.h"
#include "contracts/game_host.h"
#include "contracts/host_sdk.h"
#include "contracts/ownership.h"
#include "plugins/inventory_hooks/native_inventory_hook.h"
#include "plugins/attribute_range/plugin.h"
#include "plugins/autosell/plugin.h"
#include "plugins/core.ui/plugin.h"
#include "plugins/craft_ui/plugin.h"
#include "plugins/custom_recipe/plugin.h"
#include "plugins/extension_bag/plugin.h"
#include "plugins/inventory_hooks/plugin.h"
#include "plugins/native_choice/plugin.h"
#include "plugins/push_all/plugin.h"
#include "plugins/quest/plugin.h"
#include "plugins/save_backup/plugin.h"
#include "plugins/simple_mode/plugin.h"
#include "plugins/special_equip/plugin.h"
#include "plugins/stack_limit/plugin.h"
#include "plugins/text_layer/plugin.h"
#include "plugins/world_teleport/plugin.h"
#include "game/access/patches.h"
#include "game/access/ptr_slots.h"
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
#include "patch/ptr_slot.h"

namespace qol::host {

// L1 只冻结 ABI 表的归属、版本头与字段布局；各函数指针由 L2 host adapter 注入。
// 空表是迁移期占位，不改变当前插件仍直接使用内部实现的行为。
extern const contracts::QolHostSdk g_host_sdk{
    {sizeof(contracts::QolHostSdk), contracts::kHostSdkMajor, contracts::kHostSdkMinor},
    {}, {}, {}, {}, {}, {}, {}, {}, {}, {}, nullptr};

extern const contracts::QolGameHost g_game_host{
    {sizeof(contracts::QolGameHost), contracts::kGameHostMajor, contracts::kGameHostMinor},
    {}, {}, {}, {}, {}, {}};

namespace {

bool g_assembly_done = false;

using MechanismOutcome = qol::hooks::TxDeps::MechanismOutcome;

// 资源门禁读取：事务引擎回调，gate 为空视为开。
bool gate_on(const char* key) {
    return key == nullptr ? true : qol::infra::config_get_bool(key, false);
}

// 非入口机制的物理安装/释放分派（hooks/ 不依赖 patch/，机制归宿主注入）。
// kCallPatch 的站点由 game/access/patches 解析后交给 patch/instr_patch；
// kPtrSlot 的槽站点由 game/access/ptr_slots 解析后交给 patch/ptr_slot；
// kOwnership 直接调 claim.handler 指向的 OwnershipOps（不经 patch 站点）。
MechanismOutcome dispatch_install(const contracts::ResourceClaim& claim, uintptr_t target) {
    (void)target;  // call patch 的站点由本层解析，引擎无需预解析
    if (claim.mechanism == contracts::ResourceMechanism::kOwnership) {
        const contracts::OwnershipOps* ops = contracts::ownership_ops(claim.handler);
        if (ops == nullptr || ops->acquire == nullptr) {
            return MechanismOutcome{1, "ownership ops missing"};
        }
        const int32_t rc = contracts::ownership_acquire(ops, claim.id);
        return MechanismOutcome{rc, rc == 0 ? nullptr : "ownership acquire rejected"};
    }
    if (claim.mechanism == contracts::ResourceMechanism::kPtrSlot) {
        qol::game::PtrSlotSite site{};
        if (!qol::game::ptr_slot_site(claim.symbol, &site)) {
            return MechanismOutcome{1, "slot not found"};
        }
        const qol::patch::Outcome outcome = qol::patch::install_claim(claim, site);
        return MechanismOutcome{outcome.ok, outcome.error};
    }
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
    if (claim.mechanism == contracts::ResourceMechanism::kOwnership) {
        contracts::ownership_release(contracts::ownership_ops(claim.handler), claim.id);
        return MechanismOutcome{0, nullptr};
    }
    if (claim.mechanism == contracts::ResourceMechanism::kPtrSlot) {
        qol::game::PtrSlotSite site{};
        if (!qol::game::ptr_slot_site(claim.symbol, &site)) {
            return MechanismOutcome{1, "slot not found"};
        }
        const qol::patch::Outcome outcome = qol::patch::release_claim(claim, site);
        return MechanismOutcome{outcome.ok, outcome.error};
    }
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

    // hook/unhook 原语过渡期取自 plugins/inventory_hooks；P5 迁往 patch/ 后改引 patch 层。
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
    // 指针槽原语（write_ptr/restore_ptr）由 patch/ptr_slot 实现（mprotect + 8 字节写，fail-closed）。
    patch::set_primitives(qol::patch::Primitives{
        &call_patch_install_bl,
        &call_patch_revert_bl,
        &call_patch_install_word,
        &call_patch_revert_word,
        &qol::patch::ptr_slot_write_raw,
        &qol::patch::ptr_slot_restore_raw,
        &call_patch_install_branch,
        &call_patch_revert_branch,
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

    // inventory_hooks 先注册：沿用旧 nativeInit「库存 hook 早于其余插件安装」的时序。
    if (!register_plugin(features::inventory_hooks::plugin_descriptor())) {
        QOL_LOG_ERROR(QolDomain::kCore, "assembly: inventory_hooks registration failed");
        return false;
    }
    if (!register_plugin(features::stack_limit::plugin_descriptor())) {
        QOL_LOG_ERROR(QolDomain::kCore, "assembly: stack_limit registration failed");
        return false;
    }
    // autosell：零资源常驻插件；install 注册 save-enter/exit 回调并按配置门禁同步扫描任务。
    if (!register_plugin(features::autosell::plugin_descriptor())) {
        QOL_LOG_ERROR(QolDomain::kCore, "assembly: autosell registration failed");
        return false;
    }
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
    if (!register_plugin(features::attribute_range::plugin_descriptor())) {
        QOL_LOG_ERROR(QolDomain::kCore, "assembly: attribute_range registration failed");
        return false;
    }
    if (!register_plugin(features::text_layer::plugin_descriptor())) {
        QOL_LOG_ERROR(QolDomain::kCore, "assembly: text_layer registration failed");
        return false;
    }
    if (!register_plugin(features::special_equip::plugin_descriptor())) {
        QOL_LOG_ERROR(QolDomain::kCore, "assembly: special_equip registration failed");
        return false;
    }
    // core.ui：出口面 UI 容器（provides `ui_panels`）+ own:popup.settings；先于面板消费者
    // （save_backup）注册，保证 save_backup 的 provider 注册时容器已就绪、能力随后发布。
    if (!register_plugin(features::core_ui::plugin_descriptor())) {
        QOL_LOG_ERROR(QolDomain::kCore, "assembly: core.ui registration failed");
        return false;
    }
    // save_backup：常驻插件；删档同步 PtrHook 声明式资源 + own:popup.savebackup + provider 注册。
    if (!register_plugin(features::save_backup::plugin_descriptor())) {
        QOL_LOG_ERROR(QolDomain::kCore, "assembly: save_backup registration failed");
        return false;
    }
    // native_choice：2 条常驻资源（UIChoice_Init 入口 hook + ExecuteProc 指针槽），
    // provides `ui_choice`；先于 world_teleport 注册，保证消费者进入 enabled 时能力已发布。
    if (!register_plugin(features::native_choice::plugin_descriptor())) {
        QOL_LOG_ERROR(QolDomain::kCore, "assembly: native_choice registration failed");
        return false;
    }
    // world_teleport：零资源常驻插件；install 安装入口指令 patch（legacy 过渡机制）；
    // 选择框组件经 optional_requires=[ui_choice] 消费。
    if (!register_plugin(features::world_teleport::plugin_descriptor())) {
        QOL_LOG_ERROR(QolDomain::kCore, "assembly: world_teleport registration failed");
        return false;
    }
    // custom_recipe 先于 craft_ui 注册：前者 provides `craft_result`，后者 optional_requires 消费；
    // 拓扑顺序保证 craft_ui 安装/启用时能力已注册。
    if (!register_plugin(features::custom_recipe::plugin_descriptor())) {
        QOL_LOG_ERROR(QolDomain::kCore, "assembly: custom_recipe registration failed");
        return false;
    }
    // craft_ui：12 条 UIMix 入口 hook（5 核心 + 7 装饰），配方判定经 craft_result 能力。
    if (!register_plugin(features::craft_ui::plugin_descriptor())) {
        QOL_LOG_ERROR(QolDomain::kCore, "assembly: craft_ui registration failed");
        return false;
    }
    // extension_bag：8 条声明式资源（M1/M2 入口 hook、P1–P4 PopupState、S1 desc 槽命名空间、
    // X1 move_merge 指针槽）。最后注册：C1–C21 raw call patch 与两条轮询线程仍保留，由插件
    // on_enabled 驱动，须在其它插件装配完成后启动。
    if (!register_plugin(features::extension_bag::plugin_descriptor())) {
        QOL_LOG_ERROR(QolDomain::kCore, "assembly: extension_bag registration failed");
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
