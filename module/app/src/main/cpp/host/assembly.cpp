#include "host/assembly.h"

#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

#include "patch/call_patch.h"
#include "patch/save_enter.h"
#include "infra/log/qol_log.h"
#include "infra/capabilities.h"
#include "contracts/character_state.h"
#include "contracts/game_host.h"
#include "contracts/host_sdk.h"
#include "contracts/ownership.h"
#include "core/native/game_cache.h"
#include "game/access/inventory_legacy_read.h"
#include "game/access/save_transaction.h"
#include "game/adapter_ports/lifecycle_participant_facade.h"
#include "game/adapter_ports/save_transaction_port.h"
#include "host/legacy_json_builders.h"
#include "host/owner_cache_builder.h"
#include "host/owner_identity.h"
#include "host/save_route.h"
#include "host/config_platform_bridge.h"
#include "host/features/production.h"
#include "host/features/platform.h"
#include "host/features/http_control.h"
#include "host/http_dispatch.h"
#include "infra/slot_operation_gate.h"
#include "plugins/inventory_hooks/native_inventory_hook.h"
#include "plugins/module_health/plugin.h"
#if !QOL_KERNEL_ONLY
#include "plugins/attribute_quality/plugin.h"
#include "plugins/attribute_range_display/plugin.h"
#include "plugins/autosell/plugin.h"
#include "plugins/autosell/settings_contribution.h"
#include "plugins/settings/plugin.h"
#include "plugins/settings/settings_contribution.h"
#include "plugins/core.custom_ui/settings_contribution.h"
#include "plugins/core.custom_ui/plugin.h"
#include "plugins/core.api/plugin.h"
#include "plugins/core.craft/plugin.h"
#include "plugins/core.character/plugin.h"
#include "plugins/core.data/plugin.h"
#include "plugins/core.feature/plugin.h"
#include "plugins/custom_recipe/plugin.h"
#include "plugins/custom_recipe/settings_contribution.h"
#include "plugins/extension_bag/plugin.h"
#include "plugins/extension_bag/settings_contribution.h"
#include "plugins/inventory_hooks/plugin.h"
#include "plugins/move_merge/plugin.h"
#include "plugins/move_merge/settings_contribution.h"
#include "plugins/core.item/plugin.h"
#include "plugins/core.ui_state/plugin.h"
#include "plugins/push_all/plugin.h"
#include "plugins/push_all/settings_contribution.h"
#include "plugins/core.quest/plugin.h"
#include "plugins/save_backup/plugin.h"
#include "plugins/save_backup/settings_contribution.h"
#include "plugins/simple_mode/plugin.h"
#include "plugins/simple_mode/settings_contribution.h"
#include "plugins/special_equip/plugin.h"
#include "plugins/stack_limit/plugin.h"
#include "plugins/stack_limit/settings_contribution.h"
#include "plugins/core.system/plugin.h"
#include "plugins/character_panel_i18n/plugin.h"
#include "plugins/world_teleport/plugin.h"
#include "plugins/core.world/plugin.h"
#include "plugins/core.save/plugin.h"
#endif
#include "game/access/patches.h"
#include "game/access/qol_log_seam.h"
#include "game/state/game_state.h"
#include "game/access/ptr_slots.h"
#include "game/access/symbols.h"
#include "game/clock.h"
#include "hooks/transaction.h"
#include "host/adapters.h"
#include "host/core_settings_config_service.h"
#include "host/frame_source.h"
#include "host/item_count_facade.h"
#include "host/inventory_extension_facade.h"
#include "host/lifecycle_participants.h"
#include "host/module_health.h"
#include "host/plugin_registry.h"
#include "host/ui_adapter.h"
#include "infra/commands.h"
#include "infra/request_policy.h"
#include "infra/config.h"
#include "infra/runtime.h"
#include "infra/scheduler.h"
#include "patch/instr_patch.h"
#include "patch/ptr_slot.h"
#include "host/adapters.h"

namespace qol::host {

// T2h-B'：向 infra game_cache 注入 legacy builder（K1，行为/线程逐字不变）。
// slot 编号与 core/native/game_cache.cpp 的 CacheSlot 表一致。
// T2i-1：player/party/skills/mercenaries 四槽改指 host 内联副本（host/legacy_json_builders.h，
// 与 api/native 原实现逐字一致），两 TU 退役后仍可作插件停用 fallback；inventory(6) 的
// legacy builder 归整目录批，暂仍指 api/native。
// T2l-w-read：world 五槽（2 map / 3 units / 9 drops / 10 enemies / 11 interactives）同法改指
// host 内联（同头逐字副本），game_cache.cpp 不再 include game_world.h。
void install_legacy_cache_builders() {
    // T2n-1：向 infra 注入 world 判定 gate（game 层事实；未注入时按非 world 降级，与现状同体）。
    frame_cache_set_world_gate(&game_in_world);
    frame_cache_set_json_builder(0, &legacy_build_player_json);
    frame_cache_set_json_builder(1, &legacy_build_party_json);
    frame_cache_set_json_builder(2, &legacy_build_map_json);
    frame_cache_set_json_builder(3, &legacy_build_units_json);
    frame_cache_set_json_builder(6, &build_inventory_json);
    frame_cache_set_json_builder(7, &legacy_build_skills_json);
    frame_cache_set_json_builder(8, &legacy_build_mercenaries_json);
    frame_cache_set_json_builder(9, &legacy_build_drops_json);
    frame_cache_set_json_builder(10, &legacy_build_enemies_json);
    frame_cache_set_json_builder(11, &legacy_build_interactives_json);
}

// T2h-B1（v3 A-simple）：player(0)/party(1)/skills(7) 三槽改注入 owner 适配器
// （character.state 现取；失败逐字 fallback 同槽 legacy builder）。inventory(6)/
// mercenaries(8) 保持 legacy（归 B2/批⑥）。必须在 install_legacy_cache_builders() 之后调用。
namespace {

// 与 core.character 插件私有常量对齐（plugin.cpp: party 65536 / skills 16384 / player 512）。
constexpr uint32_t kHostStatePartyCapacity = 65536;
constexpr uint32_t kHostStateSkillsCapacity = 16384;
constexpr uint32_t kHostStatePlayerCapacity = 512;

const contracts::CharacterStateApi* require_character_state() {
    const void* iface = infra::require_capability(
        contracts::kCharacterStateCapabilityId,
        QolVersionRange{contracts::kCharacterStateVersion.major,
                        contracts::kCharacterStateVersion.minor});
    // 现取不缓存：插件撤销后 require 返回 nullptr，下一步即逐字 fallback legacy。
    return iface == nullptr ? nullptr : static_cast<const contracts::CharacterStateApi*>(iface);
}

std::string host_build_player_json() {
    return build_character_state_json_via_owner(
        require_character_state(),
        static_cast<int32_t>(contracts::QolCharacterStateKind::kPlayer), kHostStatePlayerCapacity,
        &legacy_build_player_json);
}

std::string host_build_party_json() {
    return build_character_state_json_via_owner(
        require_character_state(),
        static_cast<int32_t>(contracts::QolCharacterStateKind::kParty), kHostStatePartyCapacity,
        &legacy_build_party_json);
}

std::string host_build_skills_json() {
    return build_character_state_json_via_owner(
        require_character_state(),
        static_cast<int32_t>(contracts::QolCharacterStateKind::kSkills), kHostStateSkillsCapacity,
        &legacy_build_skills_json);
}

// T2h-B2：inventory(6) 槽 owner 适配器（item.state，现取不缓存，失败逐字 fallback legacy）。
const contracts::ItemStateApi* require_item_state() {
    const void* iface = infra::require_capability(
        contracts::kItemStateCapabilityId,
        QolVersionRange{contracts::kItemStateVersion.major, contracts::kItemStateVersion.minor});
    return iface == nullptr ? nullptr : static_cast<const contracts::ItemStateApi*>(iface);
}

std::string host_build_inventory_json() {
    return build_item_state_json_via_owner(
        require_item_state(),
        static_cast<int32_t>(contracts::QolItemStateKind::kRawInventory),
        contracts::kQolItemStateRawInventoryCapacity, &build_inventory_json);
}

}  // namespace

void install_owner_cache_builders() {
    frame_cache_set_json_builder(0, &host_build_player_json);
    frame_cache_set_json_builder(1, &host_build_party_json);
    frame_cache_set_json_builder(6, &host_build_inventory_json);
    frame_cache_set_json_builder(7, &host_build_skills_json);
}

namespace {

bool g_assembly_done = false;
uint32_t g_registered_plugins = 0;
uint32_t g_registration_failures = 0;
uint32_t g_install_failures = 0;

using PluginDescriptorFn = const contracts::PluginDescriptor& (*)();
using RegisterSettingsFn = QolResult (*)(const contracts::QolHostSdk*);

// 内建装配 sidecar；不扩展无 size 的 PluginDescriptor，也不维护第二份功能清单。
struct BuiltInPlugin {
    PluginDescriptorFn descriptor;
    const char* store_namespace = nullptr;
    bool slot_operation_allowed = false;
    bool save_route_allowed = false;
};

// 这里只登记插件描述符，不表达 provider/consumer 业务顺序；plugin_registry 根据
// requires/provides 构建 capability DAG 并决定 install/publish 顺序。
constexpr BuiltInPlugin kBuiltInPlugins[] = {
    {&features::module_health::plugin_descriptor},
#if !QOL_KERNEL_ONLY
    {&features::core_world::plugin_descriptor},
    {&features::core_craft::plugin_descriptor},
    {&features::core_save::plugin_descriptor, "module-saves", true, true},
    {&features::core_character::plugin_descriptor},
    {&features::core_data::plugin_descriptor},
    {&features::core_feature::plugin_descriptor},
    {&features::attribute_quality::plugin_descriptor},
    {&features::attribute_range_display::plugin_descriptor},
    {&features::autosell::plugin_descriptor},
    {&features::core_api::plugin_descriptor},
    {&features::core_settings::plugin_descriptor},
    {&features::core_ui::plugin_descriptor},
    {&features::custom_recipe::plugin_descriptor},
    {&features::extension_bag::plugin_descriptor},
    {&features::move_merge::plugin_descriptor},
    {&features::inventory_hooks::plugin_descriptor},
    {&features::core_item::plugin_descriptor},
    {&features::core_ui_state::plugin_descriptor},
    {&features::push_all::plugin_descriptor},
    {&features::core_quest::plugin_descriptor},
    {&features::save_backup::plugin_descriptor, nullptr, true},
    {&features::simple_mode::plugin_descriptor},
    {&features::special_equip::plugin_descriptor},
    {&features::stack_limit::plugin_descriptor},
    {&features::core_system::plugin_descriptor},
    {&features::character_panel_i18n::plugin_descriptor},
    {&features::world_teleport::plugin_descriptor},
#endif
    {nullptr},
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
// 成功时回填机制层已确证的原函数入口（outcome.backup）：kPtrSlot 为 site.expected_value
// ——install_claim 只在「槽当前值 == expected」校验通过后才写入，故该值就是被替换的原值
// （插件 wrapper 依赖它重放原逻辑）；其余机制没有原值，保持 nullptr。
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
        return MechanismOutcome{outcome.ok, outcome.error,
                                outcome.ok == 0 ? site.expected_value : nullptr};
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

#if QOL_ENABLE_DEVICE_PROBE
void probe_entry_export_resolution() {
    bool found_followed = false;
    const uintptr_t found =
        qol::game::resolve_hook_entry("elf-export:GRPX_Start", &found_followed);
    bool missing_followed = false;
    const uintptr_t missing = qol::game::resolve_hook_entry(
        "elf-export:QOL_Preflight_Missing_Export", &missing_followed);

    QOL_LOG_INFO(QolDomain::kCore,
                 "device_probe: domain=elf_export symbol=GRPX_Start result=%s expected=hit followed=%s",
                 found != 0 ? "hit" : "missing", found_followed ? "true" : "false");
    if (found == 0) {
        QOL_LOG_WARN(QolDomain::kCore,
                     "device_probe: domain=elf_export symbol=GRPX_Start diagnostic=unexpected_missing");
    }
    QOL_LOG_INFO(QolDomain::kCore,
                 "device_probe: domain=elf_export symbol=QOL_Preflight_Missing_Export result=%s expected=missing followed=%s",
                 missing != 0 ? "hit" : "missing", missing_followed ? "true" : "false");
    if (missing != 0) {
        QOL_LOG_WARN(QolDomain::kCore,
                     "device_probe: domain=elf_export symbol=QOL_Preflight_Missing_Export diagnostic=unexpected_hit");
    }
}
#endif

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
    // New-model reconciler is the sole lifecycle/frame driver (no legacy lifecycle_step/sync_gates).
    production_feature_frame_step();
    // 启动期 ownership 恢复：SaveEnter 信号驱动的单次定向重试（非逐帧/非定时）。
    // 位置固定在 reconciler 之后：desired 关闭、依赖撤销与 stop 已先经 reconciler 处理。
    feature_registry_retry_pending_ownership();
    // save_route unbind 的 drain 在帧点完成，不自旋。
    save_route::step();
    return true;
}

// SaveEnter 锁存回调：只记录当前有效 activation token，不做资源安装/插件调用/游戏访问。
void on_save_enter_ready(void* /*ctx*/) { feature_registry_notify_save_ready(); }

// save_transaction 的 gate 准入缝：gate 与 store 共用一把锁，取锁后按凭据校验，
// 消除 query→write TOCTOU。
bool save_transaction_gate_admits(void*, contracts::QolOwnerToken owner,
                                  const contracts::QolSlotOperationLease* lease, int32_t slot) {
    std::lock_guard<std::mutex> lock(infra::slot_operation::mutex());
    return contracts::succeeded(infra::slot_operation::validate_locked(owner, lease, slot));
}

}  // namespace

bool assembly_init() {
    // g_host_sdk 是 ABI 生命周期对象；adapter 在宿主装配期注入，随后由所有插件共享。
    const_cast<contracts::QolHostSdk&>(g_host_sdk).ui = ui_adapter();
    // T2l-w-write：向 game 层日志缝注入 SDK（game/access/qol_log_seam.h）；装配早于任何写调用。
    qol::game::qol_seam_bind_host_sdk(&g_host_sdk);
    // save_route 的 owner 停用通知：suspend 时旧 token 的 binding 退出派发。
    qol::host::owner_save_route_stopped_handler(&qol::host::save_route::owner_stopped);
    // P3c：runtime_hooks 的 save_transition thunk 按「core.save」插件 id 定位 owner；
    // 查询缝由 registry 提供注册表数据，save_route 不反向依赖 registry。
    qol::host::save_route::set_plugin_token_query(&qol::host::owner_token_of);
    // save_transaction 准入缝：Adapter 经宿主注入的 gate 校验 lease，不在 game 层依赖 infra。
    static const qol::game::SaveTransactionGate kSaveTransactionGate{
        nullptr, &save_transaction_gate_admits};
    qol::game::save_transaction_bind_gate(&kSaveTransactionGate);
    // 启动期 ownership 恢复信号源：一次注册（幂等），回调只锁存 owner token。
    (void)save_enter_register(&on_save_enter_ready, nullptr);
    // item_count facade 是宿主装配的常驻实现，始终可查；扩展模式由生命周期策略驱动。
    const_cast<contracts::QolGameHost&>(g_game_host).item_count = host_item_count_api();
    host_inventory_extension_bind();
    qol::game::lifecycle_participant_bind_host(host_lifecycle_dispatch());
    // 仅在完整成功后置位：失败可重试，成功则幂等。
    if (g_assembly_done) return true;
    module_health_set_fact(contracts::QolModuleHealthBit::kModuleInitialization, false, false);
    bool module_engine_failure = false;

    // 帧时钟注入：infra 不依赖 game，游戏事实由 host 装配时提供（见 infra/runtime.h）。
    infra::runtime_init(infra::Clock{&qol::game::frame_count});
    infra::set_request_op_authorizer(&security_op_enabled);

    // 保存事务 Adapter 装配期解析：SAVE_LoadCharacterAll→SAVE_Save 唯一 BL 扫描与
    // SAVE_GetSaveFileName 注入；bridge_init 已 attach ELF resolver（此处 g_base 可用）。
    qol::game::save_transaction_install();

    // 帧源常驻资源：渲染/逻辑锚点。未就绪时 fail-closed 返回 false；不阻断插件注册，
    // 后续可重试（gamebridge 在 bridge 就绪后亦会先调用一次）。
    if (!frame_source_install()) {
        QOL_LOG_WARN(QolDomain::kCore, "assembly: frame source not ready");
    }

    // 命令通道每逻辑帧排空一次；命令体在主线程执行（§6.1）。
    if (infra::scheduler_add(infra::FramePoint::kLogicPre, &drain_commands_task, nullptr, 1, 0) == 0) {
        module_engine_failure = true;
        module_health_set_fact(contracts::QolModuleHealthBit::kModuleInitialization, true, true);
    }

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

    for (const BuiltInPlugin& plugin : kBuiltInPlugins) {
        if (plugin.descriptor == nullptr) break;
        const auto& descriptor = plugin.descriptor();
        if (!register_plugin(descriptor)) {
            ++g_registration_failures;
            QOL_LOG_ERROR(QolDomain::kCore,
                          "assembly: plugin descriptor registration failed id=%s",
                          descriptor.id != nullptr ? descriptor.id : "(null)");
            return false;
        }
        if (plugin.store_namespace != nullptr &&
            !bind_plugin_store(descriptor, plugin.store_namespace)) {
            ++g_registration_failures;
            QOL_LOG_ERROR(QolDomain::kCore, "assembly: owner store binding failed");
            return false;
        }
        ++g_registered_plugins;
        // Explicit feature metadata only; a descriptor without features is a migration gap.
        const auto feature_result = production_feature_register(descriptor);
        if (!feature_result.ok()) {
            ++g_registration_failures;
            QOL_LOG_ERROR(QolDomain::kCore,
                          "assembly: feature metadata missing for %s reason=%.*s",
                          descriptor.id, static_cast<int>(feature_result.message.size()),
                          feature_result.message.c_str());
            return false;
        }
        // 授权来自装配 sidecar；registry 以注册身份定位 owner，不解释插件提交的名称，
        // 也不向插件提供授权入口。
        if (plugin.slot_operation_allowed && !bind_plugin_slot_operation(descriptor)) {
            ++g_registration_failures;
            QOL_LOG_ERROR(QolDomain::kCore, "assembly: slot operation binding failed");
            return false;
        }
        if (plugin.save_route_allowed && !bind_plugin_save_route(descriptor)) {
            ++g_registration_failures;
            QOL_LOG_ERROR(QolDomain::kCore, "assembly: save route binding failed");
            return false;
        }
    }

    // Host-owned platform/module config owner: enables /api/config/list, /api/config/set and the
    // resident lifecycle channel independent of core.settings. No module.platform enable gate.
    if (!production_feature_register_host_owner(platform_config_descriptor()).ok()) {
        ++g_registration_failures;
        QOL_LOG_ERROR(QolDomain::kCore, "assembly: platform config owner registration failed");
        module_health_set_fact(contracts::QolModuleHealthBit::kModuleInitialization, true, true);
        return false;
    }
    if (!production_feature_register_host_owner(security_config_descriptor()).ok()) {
        ++g_registration_failures;
        QOL_LOG_ERROR(QolDomain::kCore, "assembly: security config owner registration failed");
        module_health_set_fact(contracts::QolModuleHealthBit::kModuleInitialization, true, true);
        return false;
    }

    // New-model Store: init + seed from the path saved by the path-only JNI bootstrap, then bind
    // the reconciler. The single frame task below drives all lifecycle/resource reconciliation.
    module_health_config_bootstrap_started();
    const auto feature_bootstrap = production_feature_bootstrap(config_store_path());
    if (!feature_bootstrap.ok()) {
        config_store_mark_bootstrapped(false);
        QOL_LOG_ERROR(QolDomain::kCore,
                      "assembly: feature bootstrap failed reason=%.*s",
                      static_cast<int>(feature_bootstrap.message.size()),
                      feature_bootstrap.message.c_str());
    } else {
        config_store_mark_bootstrapped(true);
    }
    const bool durable_config_ready = config_store_bootstrapped() &&
        core_settings_config_service().state() == CoreSettingsConfigServiceState::kReady;
    module_health_config_bootstrap_finished(durable_config_ready);
    if (!production_feature_bind_frame(deps)) {
        ++g_registration_failures;
        QOL_LOG_ERROR(QolDomain::kCore,
                      "assembly: feature reconciler bind failed (registry/catalog mismatch or store unavailable)");
        module_health_set_fact(contracts::QolModuleHealthBit::kModuleInitialization, true, true);
        return false;
    }
    http_dispatch_bind_resident_control(&feature_control_http_route, &feature_control_http_dispatch);
    // Host-owned management channel. No permission ACL is applied (per approved design);
    // ordinary plugins interact only through the owner-scoped feature API. The ordinary
    // g_host_sdk keeps feature_control=nullptr; only the settings context receives this copy.
    contracts::QolHostSdk management_sdk = g_host_sdk;
    if (!production_feature_bind_management_sdk(management_sdk)) {
        QOL_LOG_WARN(QolDomain::kCore, "assembly: management SDK injection unavailable");
    } else {
        bind_management_sdk(management_sdk);
    }
#if QOL_ENABLE_DEVICE_PROBE
    // bridge_init attaches the ELF resolver before nativeInit enters assembly_init.
    probe_entry_export_resolution();
#endif
    g_assembly_done = true;
    module_health_set_fact(contracts::QolModuleHealthBit::kModuleInitialization, true,
                           module_engine_failure);
    QOL_LOG_INFO(QolDomain::kCore,
                 "assembly: initialized=true registered=%u install_failed=%u kernel_only=%s",
                 g_registered_plugins, g_install_failures,
                 QOL_KERNEL_ONLY ? "true" : "false");
    return true;
}

AssemblyReport assembly_report() {
    return AssemblyReport{g_registered_plugins, g_registration_failures, g_install_failures,
                          QOL_KERNEL_ONLY, g_assembly_done};
}

}  // namespace qol::host
