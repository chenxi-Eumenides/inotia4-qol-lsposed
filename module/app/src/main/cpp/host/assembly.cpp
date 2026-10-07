#include "host/assembly.h"

#include <cstdint>

#include "patch/call_patch.h"
#include "infra/log/qol_log.h"
#include "infra/capabilities.h"
#include "contracts/character_state.h"
#include "contracts/game_host.h"
#include "contracts/host_sdk.h"
#include "contracts/ownership.h"
#include "core/native/game_cache.h"
#include "game/access/inventory_legacy_read.h"
#include "game/adapter_ports/lifecycle_participant_facade.h"
#include "host/legacy_json_builders.h"
#include "host/owner_cache_builder.h"
#include "plugins/inventory_hooks/native_inventory_hook.h"
#if !QOL_KERNEL_ONLY
#include "plugins/attribute_quality/plugin.h"
#include "plugins/attribute_range_display/plugin.h"
#include "plugins/autosell/plugin.h"
#include "plugins/autosell/settings_contribution.h"
#include "plugins/core.settings/plugin.h"
#include "plugins/core.settings/settings_contribution.h"
#include "plugins/core.custom_ui/settings_contribution.h"
#include "plugins/core.custom_ui/plugin.h"
#include "plugins/core.api/plugin.h"
#include "plugins/craft_ui/plugin.h"
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
#if QOL_ENABLE_DEVICE_PROBE
#include "plugins/device_probe_consumer/plugin.h"
#include "plugins/device_probe_provider/plugin.h"
#endif
#if QOL_ENABLE_DEVICE_PROBE && !QOL_KERNEL_ONLY
#include "plugins/device_probe_export_failure/plugin.h"
#endif
#include "game/access/patches.h"
#include "game/access/qol_log_seam.h"
#include "game/state/game_state.h"
#include "game/access/ptr_slots.h"
#include "game/access/symbols.h"
#include "game/clock.h"
#include "hooks/transaction.h"
#include "host/adapters.h"
#include "host/frame_source.h"
#include "host/item_count_facade.h"
#include "host/inventory_extension_facade.h"
#include "host/lifecycle_participants.h"
#include "host/plugin_registry.h"
#include "host/ui_adapter.h"
#include "infra/commands.h"
#include "infra/config.h"
#include "infra/runtime.h"
#include "infra/scheduler.h"
#include "infra/status.h"
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
};

// 这里只登记插件描述符，不表达 provider/consumer 业务顺序；plugin_registry 根据
// requires/provides 构建 capability DAG 并决定 install/publish 顺序。
constexpr BuiltInPlugin kBuiltInPlugins[] = {
#if !QOL_KERNEL_ONLY
    {&features::core_world::plugin_descriptor},
    {&features::core_craft::plugin_descriptor},
    {&features::core_save::plugin_descriptor, "module-saves", true},
    {&features::core_character::plugin_descriptor},
    {&features::core_data::plugin_descriptor},
    {&features::core_feature::plugin_descriptor},
    {&features::attribute_quality::plugin_descriptor},
    {&features::attribute_range_display::plugin_descriptor},
    {&features::autosell::plugin_descriptor},
    {&features::core_api::plugin_descriptor},
    {&features::core_settings::plugin_descriptor},
    {&features::core_ui::plugin_descriptor},
    {&features::craft_ui::plugin_descriptor},
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
#if QOL_ENABLE_DEVICE_PROBE
    {&features::device_probe_provider::plugin_descriptor},
    {&features::device_probe_consumer::plugin_descriptor},
#endif
#if QOL_ENABLE_DEVICE_PROBE && !QOL_KERNEL_ONLY
    {&features::device_probe_export_failure::plugin_descriptor},
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
    lifecycle_step();
    sync_gates();
    return true;
}

}  // namespace

bool assembly_init() {
    // g_host_sdk 是 ABI 生命周期对象；adapter 在宿主装配期注入，随后由所有插件共享。
    const_cast<contracts::QolHostSdk&>(g_host_sdk).ui = ui_adapter();
    // T2l-w-write：向 game 层日志缝注入 SDK（game/access/qol_log_seam.h）；装配早于任何写调用。
    qol::game::qol_seam_bind_host_sdk(&g_host_sdk);
    // item_count facade 是宿主稳定表；P1-B 注册 provider 前保持 not_ready。
    const_cast<contracts::QolGameHost&>(g_game_host).item_count = host_item_count_api();
    host_inventory_extension_bind();
    qol::game::lifecycle_participant_bind_host(host_lifecycle_dispatch());
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

    for (const BuiltInPlugin& plugin : kBuiltInPlugins) {
        if (plugin.descriptor == nullptr) break;
        const auto& descriptor = plugin.descriptor();
        if (!register_plugin(descriptor)) {
            ++g_registration_failures;
            QOL_LOG_ERROR(QolDomain::kCore, "assembly: plugin descriptor registration failed");
            return false;
        }
        if (plugin.store_namespace != nullptr &&
            !bind_plugin_store(descriptor, plugin.store_namespace)) {
            ++g_registration_failures;
            QOL_LOG_ERROR(QolDomain::kCore, "assembly: owner store binding failed");
            return false;
        }
        ++g_registered_plugins;
        // 授权来自装配 sidecar；registry 以注册身份定位 owner，不解释插件提交的名称，
        // 也不向插件提供授权入口。
        if (plugin.slot_operation_allowed && !bind_plugin_slot_operation(descriptor)) {
            ++g_registration_failures;
            QOL_LOG_ERROR(QolDomain::kCore, "assembly: slot operation binding failed");
            return false;
        }
    }

    const QolResult category_result = features::core_settings::register_categories(&g_host_sdk);
    if (!contracts::succeeded(category_result)) {
        QOL_LOG_WARN(QolDomain::kCore,
                     "assembly: settings categories %s code=%d reason=%.*s",
                     category_result.code == kQolErrUnsupported ? "skipped" : "rejected",
                     category_result.code, static_cast<int>(category_result.message_len),
                     category_result.message != nullptr ? category_result.message : "");
    }
    const QolResult debug_category_result = features::core_ui::register_settings_category(&g_host_sdk);
    if (!contracts::succeeded(debug_category_result) &&
        debug_category_result.code != kQolErrUnsupported) {
        QOL_LOG_WARN(QolDomain::kCore,
                     "assembly: debug settings category rejected code=%d reason=%.*s",
                     debug_category_result.code,
                     static_cast<int>(debug_category_result.message_len),
                     debug_category_result.message != nullptr ? debug_category_result.message : "");
    }
#if QOL_ENABLE_DEVICE_PROBE
    // bridge_init attaches the ELF resolver before nativeInit enters assembly_init.
    probe_entry_export_resolution();
#endif

    // 启动期同步安装：此刻尚无帧源与在途回调，直接装完即就绪；运行期生命周期一律走事务。
    const int failed = install_all(deps);
    g_install_failures = failed > 0 ? static_cast<uint32_t>(failed) : 0;
    if (failed != 0) {
        QOL_LOG_WARN(QolDomain::kCore,
                     "assembly: registered=%u install_failed=%u kernel_only=%s",
                     g_registered_plugins, g_install_failures,
                     QOL_KERNEL_ONLY ? "true" : "false");
    }
    // 插件状态由 install_all 登记，register_items 会校验 owner 状态；贡献因此必须在其后，
    // 否则全部被判为 owner 未登记而丢弃。
    // Catalog item ordering is contribution first-success order within these categories.
    constexpr RegisterSettingsFn kSettingsContributions[]{
        &features::save_backup::register_settings,
        &features::core_settings::register_settings,
        &features::extension_bag::register_settings,
        &features::autosell::register_settings,
        &features::custom_recipe::register_settings,
        &features::stack_limit::register_settings,
        &features::push_all::register_settings,
        &features::move_merge::register_settings,
        &features::simple_mode::register_settings,
        &features::core_ui::register_settings,
    };
    for (const RegisterSettingsFn contribution : kSettingsContributions) {
        const QolResult result = contribution(&g_host_sdk);
        if (contracts::succeeded(result)) continue;
        QOL_LOG_WARN(QolDomain::kCore,
                     "assembly: settings contribution %s code=%d reason=%.*s",
                     result.code == kQolErrUnsupported ? "skipped" : "rejected", result.code,
                     static_cast<int>(result.message_len), result.message != nullptr ? result.message : "");
    }
#if QOL_ENABLE_DEVICE_PROBE && !QOL_KERNEL_ONLY
    infra::FeatureStatus export_probe_status{};
    if (infra::status_get("device_probe_export_failure", &export_probe_status)) {
        QOL_LOG_WARN(QolDomain::kCore,
                     "device_probe: plugin=%s state=%s error=%s",
                     export_probe_status.id,
                     infra::feature_state_name(export_probe_status.state),
                     export_probe_status.error != nullptr ? export_probe_status.error : "none");
    }
#endif
    g_assembly_done = true;
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
