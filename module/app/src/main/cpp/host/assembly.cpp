#include "host/assembly.h"

#include <cstdint>

#include "patch/call_patch.h"
#include "infra/log/qol_log.h"
#include "contracts/game_host.h"
#include "contracts/host_sdk.h"
#include "contracts/ownership.h"
#include "plugins/inventory_hooks/native_inventory_hook.h"
#if !QOL_KERNEL_ONLY
#include "plugins/attribute_quality/plugin.h"
#include "plugins/attribute_range_display/plugin.h"
#include "plugins/autosell/plugin.h"
#include "plugins/core.ui/plugin.h"
#include "plugins/core.api/plugin.h"
#include "plugins/craft_ui/plugin.h"
#include "plugins/character_facts/plugin.h"
#include "plugins/custom_recipe/plugin.h"
#include "plugins/extension_bag/plugin.h"
#include "plugins/inventory_hooks/plugin.h"
#include "plugins/item_facts/plugin.h"
#include "plugins/native_choice/plugin.h"
#include "plugins/push_all/plugin.h"
#include "plugins/quest/plugin.h"
#include "plugins/save_backup/plugin.h"
#include "plugins/simple_mode/plugin.h"
#include "plugins/special_equip/plugin.h"
#include "plugins/stack_limit/plugin.h"
#include "plugins/text_layer/plugin.h"
#include "plugins/world_teleport/plugin.h"
#include "plugins/world_facts/plugin.h"
#endif
#if QOL_ENABLE_DEVICE_PROBE
#include "plugins/device_probe_consumer/plugin.h"
#include "plugins/device_probe_provider/plugin.h"
#endif
#include "game/access/patches.h"
#include "game/access/ptr_slots.h"
#include "game/access/symbols.h"
#include "game/clock.h"
#include "hooks/transaction.h"
#include "host/adapters.h"
#include "host/frame_source.h"
#include "host/item_count_facade.h"
#include "host/plugin_registry.h"
#include "host/ui_adapter.h"
#include "infra/commands.h"
#include "infra/config.h"
#include "infra/runtime.h"
#include "infra/scheduler.h"
#include "patch/instr_patch.h"
#include "patch/ptr_slot.h"
#include "host/adapters.h"

namespace qol::host {

namespace {

bool g_assembly_done = false;
uint32_t g_registered_plugins = 0;
uint32_t g_registration_failures = 0;
uint32_t g_install_failures = 0;

using PluginDescriptorFn = const contracts::PluginDescriptor& (*)();

// 这里只收集描述符，不表达 provider/consumer 业务顺序；plugin_registry 根据
// requires/provides 构建 capability DAG 并决定 install/publish 顺序。
constexpr PluginDescriptorFn kPluginDescriptors[] = {
#if !QOL_KERNEL_ONLY
    &features::world_facts::plugin_descriptor,
    &features::character_facts::plugin_descriptor,
    &features::attribute_quality::plugin_descriptor,
    &features::attribute_range_display::plugin_descriptor,
    &features::autosell::plugin_descriptor,
    &features::core_api::plugin_descriptor,
    &features::core_ui::plugin_descriptor,
    &features::craft_ui::plugin_descriptor,
    &features::custom_recipe::plugin_descriptor,
    &features::extension_bag::plugin_descriptor,
    &features::inventory_hooks::plugin_descriptor,
    &features::item_facts::plugin_descriptor,
    &features::native_choice::plugin_descriptor,
    &features::push_all::plugin_descriptor,
    &features::quest::plugin_descriptor,
    &features::save_backup::plugin_descriptor,
    &features::simple_mode::plugin_descriptor,
    &features::special_equip::plugin_descriptor,
    &features::stack_limit::plugin_descriptor,
    &features::text_layer::plugin_descriptor,
    &features::world_teleport::plugin_descriptor,
#endif
#if QOL_ENABLE_DEVICE_PROBE
    &features::device_probe_provider::plugin_descriptor,
    &features::device_probe_consumer::plugin_descriptor,
#endif
    nullptr,
};

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
// 最后门禁对齐；sync_gates 处理门禁落态与 kBlocked 重评估，不会与事务中间态冲突。
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
    game_host_mark_main_thread();
    // g_host_sdk 是 ABI 生命周期对象；adapter 在宿主装配期注入，随后由所有插件共享。
    const_cast<contracts::QolHostSdk&>(g_host_sdk).ui = ui_adapter();
    // item_count facade 是宿主稳定表；P1-B 注册 provider 前保持 not_ready。
    const_cast<contracts::QolGameHost&>(g_game_host).item_count = host_item_count_api();
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
    // 本处只把机制原语注入宿主，不构成某个游戏符号的安装点。
#if !QOL_KERNEL_ONLY
    const auto hook_getter = native_hook_func;
    const auto unhook_getter = native_unhook_func;
#endif
    const qol::hooks::NativeHookFn hook_fn =
#if QOL_KERNEL_ONLY
        nullptr;
#else
        hook_getter();
#endif
    const qol::hooks::NativeUnhookFn unhook_fn =
#if QOL_KERNEL_ONLY
        nullptr;
#else
        unhook_getter();
#endif
    const HostDeps deps{
        &qol::game::resolve_hook_entry,
        hook_fn,
        unhook_fn,
    };

    // 指令 patch 原语注入：过渡期指向 patch/call_patch；P5 原语迁入 patch/ 后本处改指 patch 层。
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
        &branch_return_install,
        &branch_return_revert,
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

    for (PluginDescriptorFn descriptor_fn : kPluginDescriptors) {
        if (descriptor_fn == nullptr) break;
        if (!register_plugin(descriptor_fn())) {
            ++g_registration_failures;
            QOL_LOG_ERROR(QolDomain::kCore, "assembly: plugin descriptor registration failed");
            return false;
        }
        ++g_registered_plugins;
    }

    // 启动期同步安装：此刻尚无帧源与在途回调，直接装完即就绪；运行期生命周期一律走事务。
    const int failed = install_all(deps);
    g_install_failures = failed > 0 ? static_cast<uint32_t>(failed) : 0;
    if (failed != 0) {
        QOL_LOG_WARN(QolDomain::kCore,
                     "assembly: registered=%u install_failed=%u kernel_only=%s",
                     g_registered_plugins, g_install_failures,
                     QOL_KERNEL_ONLY ? "true" : "false");
        return false;
    }
    g_assembly_done = true;
    QOL_LOG_INFO(QolDomain::kCore,
                 "assembly: registered=%u install_failed=%u kernel_only=%s",
                 g_registered_plugins, g_install_failures,
                 QOL_KERNEL_ONLY ? "true" : "false");
    return true;
}

AssemblyReport assembly_report() {
    return AssemblyReport{g_registered_plugins, g_registration_failures, g_install_failures,
                          QOL_KERNEL_ONLY, g_assembly_done};
}

}  // namespace qol::host
