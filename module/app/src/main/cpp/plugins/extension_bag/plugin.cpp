#include "plugins/extension_bag/plugin.h"

#include <atomic>
#include <cstdint>
#include <cstring>

#include "contracts/bag.h"
#include "contracts/craft_facts.h"
#include "contracts/inventory_projection.h"
#include "contracts/inventory_refresh.h"
#include "contracts/inventory_action.h"
#include "contracts/move_merge.h"
#include "contracts/runtime_hooks.h"
#include "contracts/slot_data.h"
#include "contracts/ui.h"
#include "contracts/world_facts.h"
#include "contracts/item_facts.h"
#include "contracts/save_facts.h"
#include "plugins/extension_bag/api/bag_source.h"
#include "plugins/extension_bag/core/lifecycle.h"
#include "plugins/extension_bag/assembly/lifecycle_registration.h"
#include "plugins/extension_bag/assembly/projection_registration.h"
#include "plugins/extension_bag/extension_bag_bridge.h"
#include "plugins/extension_bag/game_ui_virtbag.h"
#include "plugins/extension_bag/runtime/inventory_access.h"
#include "plugins/extension_bag/runtime/ownership_port.h"
#include "plugins/extension_bag/runtime/session.h"

#include <cstdlib>
#include <string>

// ===========================================================================
// 扩展背包插件：袋源注册 + 声明式资源 owner
// ===========================================================================
//
// 数据面：向 core.item 注册 `inventory.bag_source` 1.1，提供扩展袋的读、
// export/import/remove 与 identify/use 原语；扩展袋数据只由 core.item 调用，
// 插件自身不对外暴露数据 API，也不读写原版/改版袋。
//
// UI 面：页签、布局/reflow、投影、绘制、输入与商店/合成/详情页面继续留在本插件；
// 投影窗口写入原版显示槽是唯一的原生袋写入例外。
//
// 资源面：入口 hook / PopupState 占用 / desc 槽命名空间与 call patch 继续声明式化；
// wrapper 业务体留在 plugins/extension_bag，本文件只做声明与门禁接线。

// M1/M2 校正（D2a-3b-1）：ADR-0007 表中 M1/M2 记为 required × failClosed；
// 按晚就绪语义校正为 required × failSoft —— 装配期失败时资源保持 free、插件
// degraded（error=资源 id），gate 开且 free 时宿主每逻辑帧重试到成功；语义等价
// 旧的 300ms 线程重试，且状态可观测。ADR-0007 与 ledger 已同步该一条校正。

namespace qol::features::extension_bag {
namespace {

constexpr const char* kPluginId = "extension_bag";
constexpr const char* kPluginVersion = "0.1.0";

const contracts::QolHostSdk* g_sdk = nullptr;
const contracts::QolGameHost* g_game = nullptr;
const contracts::QolExtensionBagWritePort* g_write_port = nullptr;
const contracts::ItemFactsApi* g_item_facts = nullptr;
const contracts::CraftFactsApi* g_craft_facts = nullptr;

// 生命周期参与者的可用探针：进入 enabled 时置位，installed_disabled 与
// deactivate 后清除。袋源注册状态由 api/bag_source.* 单独维护。
std::atomic<bool> g_source_active{false};
bool g_lifecycle_participant_registered = false;

// 存档/UI 生命周期参与者回调：可用性由 g_source_active 承载，动作回调直连扩展实现。
// 调用方（宿主分派器）已按 available 门控，本表回调只在可用时执行。
int32_t lifecycle_available() { return g_source_active.load(std::memory_order_acquire) ? 1 : 0; }
int32_t lifecycle_prepare_save_slot_load() {
    return virtual_bag_prepare_save_slot_load() ? 1 : 0;
}
int32_t lifecycle_is_inventory_enter(uintptr_t enter) {
    return virtual_bag_is_inventory_enter(enter) ? 1 : 0;
}
int32_t lifecycle_prepare_main_menu() {
    return virtual_bag_prepare_main_menu() ? 1 : 0;
}
int32_t lifecycle_inventory_state_id() { return virtual_bag_inventory_state_id(); }

const qol::contracts::QolLifecycleParticipant kLifecycleParticipant{
    sizeof(qol::contracts::QolLifecycleParticipant),
    qol::contracts::kLifecycleParticipantAbiVersion,
    &lifecycle_available,
    &lifecycle_prepare_save_slot_load,
    &lifecycle_is_inventory_enter,
    &lifecycle_prepare_main_menu,
    &lifecycle_inventory_state_id,
};


// 资源表在 extension_bag_claims() 首次调用时构造（函数内静态，无跨 TU 静态初始化顺序问题）。
uint32_t g_resource_count = 0;
const qol::contracts::ResourceClaim* g_resources = extension_bag_claims(&g_resource_count);

// 本插件不再向宿主提供 extension_bag / inventory.merge_projection 能力：
// 数据面经 core.item 的 inventory.bag_source 注册，投影回调改为注册到 core.item。

const qol::contracts::CapabilityRequirement kRequires[] = {
    {
        qol::contracts::kWorldFactsCapabilityId,
        {qol::contracts::kWorldFactsVersionMajor, qol::contracts::kWorldFactsVersionMinor},
    },
    {
        qol::contracts::kItemFactsCapabilityId,
        {qol::contracts::kItemFactsVersionMajor, qol::contracts::kItemFactsVersionMinor},
    },
    {
        qol::contracts::kSaveFactsCapabilityId,
        {qol::contracts::kSaveFactsVersionMajor, qol::contracts::kSaveFactsVersionMinor},
    },
    {
        qol::contracts::kCraftFactsCapabilityId,
        {qol::contracts::kCraftFactsVersionMajor, qol::contracts::kCraftFactsVersionMinor},
    },
    // P3c：扩展背包数据面依赖 save.slot_data（SessionBound 读 + SlotBound 写，owner=
    // PluginContext 注入 token）；旧 ModuleSaveStore/Coordinator 生产链退役。
    {
        qol::contracts::kQolSlotDataCapabilityId,
        {qol::contracts::kQolSlotDataVersion.major, qol::contracts::kQolSlotDataVersion.minor},
    },
    // 袋源注册面：core.item 提供，扩展背包 activate 时注册、deactivate 时注销。
    {
        qol::contracts::kBagSourceCapabilityId,
        {qol::contracts::kBagSourceVersion.major, qol::contracts::kBagSourceVersion.minor},
    },
    {
        qol::contracts::kInventoryActionCapabilityId,
        {qol::contracts::kInventoryActionVersionMajor,
         qol::contracts::kInventoryActionVersionMinor},
    },
    // 拖拽/合并回调注册在 core.item 的注册表；缺它时扩展页只能显示，不能完成跨袋移动。
    {
        qol::contracts::kInventoryProjectionCapabilityId,
        {qol::contracts::kInventoryProjectionVersionMajor,
         qol::contracts::kInventoryProjectionVersionMinor},
    },
};

const qol::contracts::CapabilityRequirement kOptionalRequires[] = {
    {qol::contracts::kInventoryRefreshCapabilityId,
     {qol::contracts::kInventoryRefreshVersionMajor,
      qol::contracts::kInventoryRefreshVersionMinor}},
    {qol::contracts::kMoveMergeControlCapabilityId,
     {qol::contracts::kMoveMergeControlVersionMajor,
      qol::contracts::kMoveMergeControlVersionMinor}},
    // 页签锚定：句柄存活/身份校验/失效重建由 core.custom_ui 的 ui_components 提供。
    {qol::contracts::kUiComponentsCapabilityId,
     {qol::contracts::kUiContractMajor, qol::contracts::kUiContractMinor}},
};

const qol::contracts::QolPortRequirement kPorts[] = {
    {qol::contracts::QolPortId::QOL_PORT_EXTENSION_BAG_WRITE,
     {qol::contracts::kQolPortExtensionBagWriteVersion.major,
      qol::contracts::kQolPortExtensionBagWriteVersion.minor}},
    {qol::contracts::QolPortId::QOL_PORT_CHARACTER_EXTRAS,
     {qol::contracts::kQolPortCharacterExtrasVersion.major,
      qol::contracts::kQolPortCharacterExtrasVersion.minor}},
    {qol::contracts::QolPortId::QOL_PORT_INVENTORY_EXTRAS,
     {qol::contracts::kQolPortInventoryExtrasVersion.major,
      qol::contracts::kQolPortInventoryExtrasVersion.minor}},
};

// P3c：SlotBound dirty 的锁外安全点排空（RenderPre 每帧）；save.slot_data 的 iface
// 在 install 时 require 后缓存（capability 与插件同生命周期，宿主 revoke 时先于本
// 插件停用 provider——表指针由宿主 capability 表持有，此处不管理其生命周期）。
uint64_t g_slot_data_flush_task = 0;
uint64_t g_deferred_free_task = 0;
const qol::contracts::QolSlotDataApi* g_slot_data_api_cache = nullptr;

void close_extension_bag_gates() {
    g_source_active.store(false, std::memory_order_release);
    extension_bag_bag_source_unregister();
    extension_bag_set_enabled(false);
}

// Resident callbacks are gated off on install failure; only reversible registrations
// (slot_data / bag source) are revoked.
void rollback_extension_bag_install(const qol::contracts::QolSlotDataApi* slot_data_api = nullptr) {
    close_extension_bag_gates();
    if (slot_data_api == nullptr) slot_data_api = g_slot_data_api_cache;
    if (slot_data_api != nullptr) (void)extension_bag_slot_data_unregister(slot_data_api);
    g_slot_data_api_cache = nullptr;
    extension_bag_bind_port_sdk(nullptr);
    extension_bag_bind_item_facts(nullptr);
    extension_bag_bind_craft_facts(nullptr);
    extension_bag_bind_character_extras(nullptr);
    extension_bag_bind_inventory_extras(nullptr);
    extension_bag_bind_world_facts(nullptr);
    g_item_facts = nullptr;
    extension_bag_bind_save_facts(nullptr);
    g_craft_facts = nullptr;
    g_sdk = nullptr;
    g_game = nullptr;
    g_write_port = nullptr;
}

int32_t slot_data_flush_frame(int64_t, void*) {
    extension_bag_flush_dirty_staging(g_slot_data_api_cache);
    return 1;
}

bool register_bag_source_if_available() {
    if (g_sdk == nullptr || g_sdk->capabilities.require == nullptr) return false;
    const void* iface = nullptr;
    const QolResult result = g_sdk->capabilities.require(
        qol::contracts::kBagSourceCapabilityId,
        {qol::contracts::kBagSourceVersion.major, qol::contracts::kBagSourceVersion.minor},
        &iface);
    if (!qol::contracts::succeeded(result) || iface == nullptr) return false;
    return extension_bag_bag_source_register(
        static_cast<const qol::contracts::QolBagSourceRegistryApi*>(iface));
}

int plugin_install(qol::contracts::PluginContext& ctx) {
    if (extension_bag_bag_source_registered() || g_slot_data_api_cache != nullptr) {
        rollback_extension_bag_install();
    } else {
        close_extension_bag_gates();
    }
    g_sdk = ctx.sdk;
    g_game = ctx.game;
    g_write_port = nullptr;
    extension_bag_bind_character_extras(nullptr);
    extension_bag_bind_inventory_extras(nullptr);
    extension_bag_bind_world_facts(nullptr);
    g_item_facts = nullptr;
    extension_bag_bind_save_facts(nullptr);
    g_craft_facts = nullptr;
    extension_bag_bind_character_extras(nullptr);
    extension_bag_bind_inventory_extras(nullptr);
    if (ctx.struct_size < sizeof(qol::contracts::PluginContext) ||
        ctx.sdk_major != qol::contracts::kHostSdkMajor ||
        ctx.sdk == nullptr || ctx.game == nullptr) {
        g_sdk = nullptr;
        g_game = nullptr;
        g_write_port = nullptr;
    extension_bag_bind_character_extras(nullptr);
    extension_bag_bind_inventory_extras(nullptr);
        extension_bag_bind_character_extras(nullptr);
        extension_bag_bind_inventory_extras(nullptr);
        extension_bag_bind_port_sdk(nullptr);
        extension_bag_bind_item_facts(nullptr);
        extension_bag_bind_craft_facts(nullptr);
        return 1;
    }
    if (ctx.ports == nullptr || ctx.port_count != 3 || ctx.ports[0] == nullptr ||
        ctx.ports[1] == nullptr || ctx.ports[2] == nullptr) {
        g_sdk = nullptr;
        g_game = nullptr;
        g_write_port = nullptr;
    extension_bag_bind_character_extras(nullptr);
    extension_bag_bind_inventory_extras(nullptr);
        extension_bag_bind_character_extras(nullptr);
        extension_bag_bind_inventory_extras(nullptr);
        extension_bag_bind_port_sdk(nullptr);
        extension_bag_bind_item_facts(nullptr);
        extension_bag_bind_craft_facts(nullptr);
        return 1;
    }
    g_write_port = static_cast<const qol::contracts::QolExtensionBagWritePort*>(ctx.ports[0]);
    extension_bag_bind_character_extras(
        static_cast<const qol::contracts::QolCharacterExtrasPort*>(ctx.ports[1]));
    extension_bag_bind_inventory_extras(
        static_cast<const qol::contracts::QolInventoryExtrasPort*>(ctx.ports[2]));
    const void* world_facts_iface = nullptr;
    if (ctx.sdk->capabilities.require == nullptr ||
        !qol::contracts::succeeded(ctx.sdk->capabilities.require(
            qol::contracts::kWorldFactsCapabilityId,
            {qol::contracts::kWorldFactsVersionMajor, qol::contracts::kWorldFactsVersionMinor},
            &world_facts_iface)) ||
        world_facts_iface == nullptr) {
        g_sdk = nullptr;
        g_game = nullptr;
        g_write_port = nullptr;
    extension_bag_bind_character_extras(nullptr);
    extension_bag_bind_inventory_extras(nullptr);
        extension_bag_bind_character_extras(nullptr);
        extension_bag_bind_inventory_extras(nullptr);
        extension_bag_bind_port_sdk(nullptr);
        extension_bag_bind_item_facts(nullptr);
        extension_bag_bind_craft_facts(nullptr);
        return 1;
    }
    extension_bag_bind_world_facts(static_cast<const qol::contracts::WorldFactsApi*>(world_facts_iface));
    const void* item_facts_iface = nullptr;
    if (!qol::contracts::succeeded(ctx.sdk->capabilities.require(
            qol::contracts::kItemFactsCapabilityId,
            {qol::contracts::kItemFactsVersionMajor, qol::contracts::kItemFactsVersionMinor},
            &item_facts_iface)) ||
        item_facts_iface == nullptr || !qol::contracts::item_facts_api_size_valid(
                                           static_cast<const qol::contracts::ItemFactsApi*>(item_facts_iface))) {
        g_sdk = nullptr;
        g_game = nullptr;
        g_write_port = nullptr;
    extension_bag_bind_character_extras(nullptr);
    extension_bag_bind_inventory_extras(nullptr);
        extension_bag_bind_character_extras(nullptr);
        extension_bag_bind_inventory_extras(nullptr);
        extension_bag_bind_world_facts(nullptr);
        extension_bag_bind_port_sdk(nullptr);
        extension_bag_bind_item_facts(nullptr);
        extension_bag_bind_craft_facts(nullptr);
        return 1;
    }
    g_item_facts = static_cast<const qol::contracts::ItemFactsApi*>(item_facts_iface);
    const void* save_facts_iface = nullptr;
    if (!qol::contracts::succeeded(ctx.sdk->capabilities.require(
            qol::contracts::kSaveFactsCapabilityId,
            {qol::contracts::kSaveFactsVersionMajor, qol::contracts::kSaveFactsVersionMinor},
            &save_facts_iface)) ||
        save_facts_iface == nullptr) {
        g_sdk = nullptr;
        g_game = nullptr;
        g_write_port = nullptr;
    extension_bag_bind_character_extras(nullptr);
    extension_bag_bind_inventory_extras(nullptr);
        extension_bag_bind_character_extras(nullptr);
        extension_bag_bind_inventory_extras(nullptr);
        extension_bag_bind_world_facts(nullptr);
        g_item_facts = nullptr;
        extension_bag_bind_save_facts(nullptr);
        extension_bag_bind_port_sdk(nullptr);
        extension_bag_bind_item_facts(nullptr);
        extension_bag_bind_craft_facts(nullptr);
        return 1;
    }
    extension_bag_bind_save_facts(static_cast<const qol::contracts::SaveFactsApi*>(save_facts_iface));
    const void* craft_facts_iface = nullptr;
    if (!qol::contracts::succeeded(ctx.sdk->capabilities.require(
            qol::contracts::kCraftFactsCapabilityId,
            {qol::contracts::kCraftFactsVersionMajor, qol::contracts::kCraftFactsVersionMinor},
            &craft_facts_iface)) ||
        craft_facts_iface == nullptr) {
        g_sdk = nullptr;
        g_game = nullptr;
        g_write_port = nullptr;
    extension_bag_bind_character_extras(nullptr);
    extension_bag_bind_inventory_extras(nullptr);
        extension_bag_bind_character_extras(nullptr);
        extension_bag_bind_inventory_extras(nullptr);
        extension_bag_bind_world_facts(nullptr);
        g_item_facts = nullptr;
        extension_bag_bind_save_facts(nullptr);
        g_craft_facts = nullptr;
        extension_bag_bind_port_sdk(nullptr);
        extension_bag_bind_item_facts(nullptr);
        extension_bag_bind_craft_facts(nullptr);
        return 1;
    }
    g_craft_facts = static_cast<const qol::contracts::CraftFactsApi*>(craft_facts_iface);
    extension_bag_bind_item_facts(g_item_facts);
    extension_bag_bind_craft_facts(g_craft_facts);
    if (!extension_bag_bind_host(g_sdk, g_game, g_write_port)) {
        g_sdk = nullptr;
        g_game = nullptr;
        g_write_port = nullptr;
    extension_bag_bind_character_extras(nullptr);
    extension_bag_bind_inventory_extras(nullptr);
        extension_bag_bind_character_extras(nullptr);
        extension_bag_bind_inventory_extras(nullptr);
        extension_bag_bind_port_sdk(nullptr);
        extension_bag_bind_item_facts(nullptr);
        extension_bag_bind_craft_facts(nullptr);
        return 1;
    }
    extension_bag_bind_port_sdk(g_sdk);
    // 生命周期帧任务必须尽早安装：安装期 SDK 的 scheduler 可能尚未就绪，
    // 后续资源就绪回调会再次尝试（函数幂等）。
    ensure_lifecycle_task();
    // 袋源是 core.item 访问扩展袋的唯一入口；注册失败时插件没有可用数据面，
    // 不能以 enabled 状态继续运行。
    if (!register_bag_source_if_available()) {
        if (g_sdk != nullptr && g_sdk->log.write != nullptr) {
            constexpr char kMessage[] =
                "extension bag bag_source registration failed; install rejected";
            (void)g_sdk->log.write(2, 1, kPluginId, kMessage,
                                   static_cast<uint32_t>(sizeof(kMessage) - 1));
        }
        rollback_extension_bag_install();
        return 1;
    }
    // 生命周期参与者最末注册。注册表 set-once；失败时保持不可用（active=false）并登记
    // 部分注册状态，install 返回失败。
    if (g_sdk == nullptr || !detail::register_lifecycle_participant_once(
                                g_sdk->lifecycle_participant_register, &kLifecycleParticipant,
                                g_lifecycle_participant_registered)) {
        if (g_sdk != nullptr && g_sdk->log.write != nullptr) {
            constexpr char kMessage[] =
                "extension bag lifecycle participant registration failed; source unavailable";
            (void)g_sdk->log.write(2, 1, kPluginId, kMessage,
                                   static_cast<uint32_t>(sizeof(kMessage) - 1));
        }
        rollback_extension_bag_install();
        return 1;
    }
    // P3c：扩展背包数据面接入 save.slot_data（owner=PluginContext 注入 token；宿主按
    // 真实注册身份注入，插件不自报）。注册失败即 install 失败——数据面缺失时投影/事务
    // 仍可运行，但持久化断裂比拒绝启用更危险（fail-closed）。
    const void* slot_data_iface = nullptr;
    if (g_sdk->capabilities.require == nullptr ||
        !qol::contracts::succeeded(g_sdk->capabilities.require(
            qol::contracts::kQolSlotDataCapabilityId,
            {qol::contracts::kQolSlotDataVersion.major, qol::contracts::kQolSlotDataVersion.minor},
            &slot_data_iface)) ||
        slot_data_iface == nullptr) {
        rollback_extension_bag_install();
        return 1;
    }
    const auto* slot_data_api = static_cast<const qol::contracts::QolSlotDataApi*>(slot_data_iface);
    if (!extension_bag_slot_data_register(ctx.owner_token, slot_data_api)) {
        rollback_extension_bag_install(slot_data_api);
        return 1;
    }
    g_slot_data_api_cache = slot_data_api;
    if (g_sdk->scheduler.add != nullptr && g_slot_data_flush_task == 0) {
        uint64_t task = 0;
        if (contracts::succeeded(g_sdk->scheduler.add(qol::contracts::kQolRuntimeFramePointRenderPre,
                                                      &slot_data_flush_frame, nullptr, 1, 0,
                                                      &task))) {
            g_slot_data_flush_task = task;
        }
    }
    return 0;
}

void plugin_deactivate(qol::contracts::PluginContext& ctx) {
    (void)ctx;
    // 先注销袋源与投影回调：注销后 core.item 不再回调本插件，随后才拆绑定。
    extension_bag_bag_source_unregister();
    unregister_projection_callbacks();
    // P3c：先关 slot_data 准入（unregister 先拒绝新请求；终态后零回调由 provider 保证），
    // 磁盘 section 保留（不自动删除、不隐式保存）；注册请求在 deactivate（主线程）内
    // 经当帧 pump 收敛，返回 request 仅供诊断。
    if (g_sdk != nullptr && g_sdk->capabilities.require != nullptr) {
        const void* slot_data_iface = nullptr;
        if (contracts::succeeded(g_sdk->capabilities.require(
                qol::contracts::kQolSlotDataCapabilityId,
                {qol::contracts::kQolSlotDataVersion.major,
                 qol::contracts::kQolSlotDataVersion.minor},
                &slot_data_iface)) &&
            slot_data_iface != nullptr) {
            (void)extension_bag_slot_data_unregister(
                static_cast<const qol::contracts::QolSlotDataApi*>(slot_data_iface));
        }
    }
    g_slot_data_api_cache = nullptr;
    if (g_slot_data_flush_task != 0 && g_sdk != nullptr && g_sdk->scheduler.remove != nullptr) {
        (void)g_sdk->scheduler.remove(g_slot_data_flush_task);
        g_slot_data_flush_task = 0;
    }
    if (g_deferred_free_task != 0) {
        if (g_sdk != nullptr && g_sdk->scheduler.remove != nullptr) {
            (void)g_sdk->scheduler.remove(g_deferred_free_task);
        }
        g_deferred_free_task = 0;
    }
    // Resources are returned by the host; this callback only closes feature gates.
    g_source_active.store(false, std::memory_order_release);
    extension_bag_set_enabled(false);
    extension_bag_bind_world_facts(nullptr);
    g_item_facts = nullptr;
    extension_bag_bind_save_facts(nullptr);
    g_craft_facts = nullptr;
    extension_bag_bind_item_facts(nullptr);
    extension_bag_bind_craft_facts(nullptr);
    g_game = nullptr;
    g_write_port = nullptr;
    extension_bag_bind_character_extras(nullptr);
    extension_bag_bind_inventory_extras(nullptr);
    g_sdk = nullptr;
}

// 启停仅切门禁，资源安装/还原由宿主完成（§3.3）。
void plugin_on_enabled(qol::contracts::PluginContext& ctx) {
    close_extension_bag_gates();
    // P3c 生命周期：启停会经 owner suspend/resume 换发 token；此处以本次 ctx.owner_token
    // 重新注册 slot_data（register 内部先注销旧凭据参与者），使 CB 回调的 owner 校验
    // 始终对应当前有效 token（§9 双凭据迁移）。
    if (g_slot_data_api_cache == nullptr ||
        !extension_bag_slot_data_register(ctx.owner_token, g_slot_data_api_cache)) {
        if (g_sdk != nullptr && g_sdk->log.write != nullptr) {
            constexpr char kMessage[] = "extension bag slot_data re-register on enable failed";
            (void)g_sdk->log.write(2, 1, kPluginId, kMessage,
                                   static_cast<uint32_t>(sizeof(kMessage) - 1));
        }
        return;
    }
    if (g_sdk != nullptr && g_sdk->scheduler.add != nullptr && g_deferred_free_task == 0) {
        uint64_t task = 0;
        if (contracts::succeeded(g_sdk->scheduler.add(
                qol::contracts::kQolRuntimeFramePointLogicPre,
                &extension_bag_deferred_free_tick, nullptr, 1, 0, &task)) && task != 0) {
            g_deferred_free_task = task;
        }
    }
    // enable 会重新注册袋源，保证 core.item 的注册表代次变化、快照失效。
    if (!register_bag_source_if_available()) {
        if (g_sdk != nullptr && g_sdk->log.write != nullptr) {
            constexpr char kMessage[] = "extension bag bag_source re-register on enable failed";
            (void)g_sdk->log.write(2, 1, kPluginId, kMessage,
                                   static_cast<uint32_t>(sizeof(kMessage) - 1));
        }
        extension_bag_set_enabled(false);
        g_source_active.store(false, std::memory_order_release);
        return;
    }
    // 投影回调是跨袋移动/合并的唯一 UI 适配入口；注册失败时数据面仍可用，
    // 但拖拽会退化，必须让插件保持关闭而不是暴露半可用状态。
    const QolResult projection_result = register_projection_callbacks();
    if (!contracts::succeeded(projection_result)) {
        if (g_sdk != nullptr && g_sdk->log.write != nullptr) {
            constexpr char kMessage[] = "extension bag projection registration on enable failed";
            (void)g_sdk->log.write(2, 1, kPluginId, kMessage,
                                   static_cast<uint32_t>(sizeof(kMessage) - 1));
        }
        extension_bag_bag_source_unregister();
        extension_bag_set_enabled(false);
        g_source_active.store(false, std::memory_order_release);
        return;
    }
    g_source_active.store(true, std::memory_order_release);
    extension_bag_set_enabled(true);
}

void plugin_on_disabled(qol::contracts::PluginContext& ctx) {
    // 停用但保留注册句柄：注销 slot_data（先关准入；磁盘 section 保留，不隐式保存）。
    if (g_slot_data_api_cache != nullptr) {
        (void)extension_bag_slot_data_unregister(g_slot_data_api_cache);
    }
    (void)ctx;
    extension_bag_bag_source_unregister();
    unregister_projection_callbacks();
    g_source_active.store(false, std::memory_order_release);
    extension_bag_set_enabled(false);
}

void plugin_fill_status(qol::contracts::PluginStatusOut& out) {
    out.enabled = qol::features::extension_bag::extension_bag_enabled();
    out.error = nullptr;
}

int32_t plugin_on_resource_ready(qol::contracts::PluginContext& ctx, const char* resource_id,
                                 void* backup) {
    (void)ctx;
    // 资源就绪阶段 SDK/scheduler 已可用，补装安装期可能失败的生命周期任务。
    ensure_lifecycle_task();
    // 受限回调（§3.3）：只缓存入口 backup（常数时间），无资源操作。
    return extension_bag_bind_backup(resource_id, backup);
}

// 单一 feature：默认 true（保持旧默认启用行为），用户可见开关。插件仅在 feature
// 开启（want_run）时安装；资源 gate=nullptr 跟随 lifecycle。
QolResult extension_bag_toggle(contracts::PluginContext&, uint8_t enabled) {
    if (enabled != 0) {
        extension_bag_set_enabled(true);
        // 重新注册使 core.item 的注册表代次变化、快照失效。
        if (!register_bag_source_if_available()) {
            extension_bag_set_enabled(false);
            g_source_active.store(false, std::memory_order_release);
            return contracts::error(kQolErrNotReady, "bag source unavailable", 22);
        }
        const QolResult projection_result = register_projection_callbacks();
        if (!contracts::succeeded(projection_result)) {
            extension_bag_bag_source_unregister();
            extension_bag_set_enabled(false);
            g_source_active.store(false, std::memory_order_release);
            return projection_result;
        }
        g_source_active.store(true, std::memory_order_release);
    } else {
        extension_bag_bag_source_unregister();
        unregister_projection_callbacks();
        extension_bag_set_enabled(false);
        g_source_active.store(false, std::memory_order_release);
    }
    return contracts::ok();
}

constexpr qol::contracts::FeatureDefinition kFeatures[] = {
    {"extension_bag.extended_bag", 1, qol::contracts::FeatureCategory::kGameplay,
     qol::contracts::FeatureType::kToggle, "额外背包", "添加一列额外背包",
      &extension_bag_toggle, nullptr, nullptr, -2},
};

const qol::contracts::PluginDescriptor kDescriptor = {
    kPluginId,
    kPluginVersion,
    8, kRequires,  // requires=world.facts + item.facts + save.facts + craft.facts + save.slot_data + inventory.bag_source + inventory.action 3.0 + inventory.projection 1.0
    static_cast<uint32_t>(sizeof(kOptionalRequires) / sizeof(kOptionalRequires[0])),
    kOptionalRequires,
    0, nullptr,
    g_resource_count, g_resources,
    // Intentional global bridge ownership: extension_bag JNI exports live in bridge/native.
    // check_jni validates bridge declarations against exports both ways; descriptor reverse
    // coverage is not implemented.
    0, nullptr,
    &plugin_install,
    &plugin_deactivate,
    &plugin_on_enabled,
    &plugin_on_disabled,
    &plugin_fill_status,
    &plugin_on_resource_ready,
    kPorts,
    3,
    nullptr, 0,
    kFeatures, 1,
    nullptr,
    0,
    nullptr,
    nullptr, 0,
};

}  // namespace

const qol::contracts::PluginDescriptor& plugin_descriptor() {
    return kDescriptor;
}

}  // namespace qol::features::extension_bag
