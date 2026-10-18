#include "plugins/extension_bag/plugin.h"

#include <atomic>
#include <cstdint>
#include <cstring>

#include "contracts/events.h"
#include "contracts/craft_facts.h"
#include "contracts/http.h"
#include "contracts/inventory_refresh.h"
#include "contracts/move_merge.h"
#include "contracts/runtime_hooks.h"
#include "contracts/slot_data.h"
#include "contracts/world_facts.h"
#include "contracts/item_facts.h"
#include "contracts/save_facts.h"
#include "plugins/extension_bag/extension_bag_bridge.h"
#include "plugins/extension_bag/extension_bag_context.h"
#include "plugins/extension_bag/game_ui_virtbag.h"
#include "plugins/extension_bag/lifecycle_registration.h"
#include "plugins/extension_bag/move_merge.h"
#include "plugins/extension_bag/http/routes.h"

#include <cstdlib>
#include <string>

// ===========================================================================
// 扩展背包插件：声明式资源 owner + extension_bag provider + projection capability
// ===========================================================================
//
// 资源面（a3b-1/a3b-2）：入口 hook / PopupState 占用 / desc 槽命名空间
// 与 C1–C21 call patch 全部声明式化；wrapper 业务体逐字留在 plugins/extension_bag
// （D2c-2b-2 后 feature/patch 已删除），本文件只做声明与门禁接线。
//
// 能力面（a3c）：provides `extension_bag` v3.1（ExtensionBagApi 保留 v3.0 前缀并尾追加卖出删除），
// iface 由 extension_bag_api_table() 提供（EB TU 逐条转发既有 port）。宿主在 enabled 时注册、
// revoking 时注销（§3.4.4 既有路径）；本文件不新增 JNI / 资源。
//
// Move-merge projection bridge; the move_merge plugin owns the independent gate and X1 claim.
//
// M1/M2 校正（D2a-3b-1）：ADR-0007 表中 M1/M2 记为 required × failClosed；
// 按晚就绪语义校正为 required × failSoft —— 装配期失败时资源保持 free、插件
// degraded（error=资源 id），gate 开且 free 时宿主每逻辑帧重试到成功；语义等价
// 旧的 300ms 线程重试，且状态可观测。ADR-0007 与 ledger 已同步该一条校正。

namespace qol::features::extension_bag {
namespace {

constexpr const char* kPluginId = "extension_bag";
constexpr const char* kPluginVersion = "0.1.0";
bool g_result_handler_registered = false;

const contracts::QolHostSdk* g_sdk = nullptr;
const contracts::QolGameHost* g_game = nullptr;
const contracts::QolExtensionBagWritePort* g_write_port = nullptr;
const contracts::WorldFactsApi* g_world_facts = nullptr;
const contracts::ItemFactsApi* g_item_facts = nullptr;
const contracts::SaveFactsApi* g_save_facts = nullptr;
const contracts::CraftFactsApi* g_craft_facts = nullptr;

// 中立 provider / 生命周期参与者的可用探针：进入 enabled 时置位，installed_disabled 与
// deactivate 后清除（复刻旧“能力仅在 enabled 时注册”的可用语义）。表本身静态驻留（set-once），
// active 只表达是否参与路由/分派，不表示登记与否。
std::atomic<bool> g_provider_active{false};
bool g_lifecycle_participant_registered = false;

// 存档/UI 生命周期参与者回调：可用性由 provider active 承载，动作回调直连扩展实现。
// 调用方（宿主分派器）已按 available 门控，本表回调只在可用时执行。
int32_t lifecycle_available() { return g_provider_active.load(std::memory_order_acquire) ? 1 : 0; }
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


bool merge_projection_original_item_input_blocked() {
    return merge_projection_available() && virtual_bag_original_item_input_blocked();
}

bool merge_projection_allow_original_tab_drop(void* control, void* source_control,
                                               uint64_t caller_offset) {
    return merge_projection_available() &&
           virtual_bag_allow_original_tab_drop(control, source_control, caller_offset);
}

uint64_t merge_projection_observe_pre(void* control, uint64_t event, void* x2, void* param) {
    return merge_projection_available()
               ? virtual_bag_observe_item_proc_pre(control, event, x2, param)
               : 0;
}

void merge_projection_observe_source(uint64_t observation_token, void* control, uint64_t event,
                                     void* x2, void* param, void* source_control) {
    if (merge_projection_available()) {
        virtual_bag_observe_item_proc_source(observation_token, control, event, x2, param,
                                             source_control);
    }
}

void merge_projection_observe_post(uint64_t observation_token, void* control, uint64_t event,
                                   void* x2, void* param, uint64_t result) {
    if (merge_projection_available()) {
        virtual_bag_observe_item_proc_post(observation_token, control, event, x2, param, result);
    }
}

bool merge_projection_module_view_installed() {
    return merge_projection_available() && virtual_bag_module_view_installed();
}

bool merge_projection_sync_slot(int32_t display_bag, int32_t slot) {
    return merge_projection_available() && virtual_bag_sync_projected_slot(display_bag, slot);
}

bool merge_projection_sync_bag() {
    return merge_projection_available() && virtual_bag_sync_projected_bag();
}

const qol::contracts::InventoryMergeProjectionApi kMergeProjectionApi{
    sizeof(qol::contracts::InventoryMergeProjectionApi),
    qol::contracts::kInventoryMergeProjectionVersionMajor,
    qol::contracts::kInventoryMergeProjectionVersionMinor,
    &merge_projection_drop,
    &merge_projection_original_item_input_blocked,
    &merge_projection_allow_original_tab_drop,
    &merge_projection_identify_extension_item,
    &merge_projection_observe_pre,
    &merge_projection_observe_source,
    &merge_projection_observe_post,
    &merge_projection_module_view_installed,
    &merge_projection_sync_slot,
    &merge_projection_sync_bag,
};

// 资源表在 extension_bag_claims() 首次调用时构造（函数内静态，无跨 TU 静态初始化顺序问题）。
uint32_t g_resource_count = 0;
const qol::contracts::ResourceClaim* g_resources = extension_bag_claims(&g_resource_count);

// 能力提供声明：extension_bag v3.1，iface = EB TU 的 ExtensionBagApi 静态表。
// 注册失败（iface 空/容量满/重复）由宿主只记录不阻断（plugin_registry 既有口径）。
const qol::contracts::CapabilityProvisionRef kProvides[] = {
    {
        qol::contracts::kExtensionBagCapabilityId,
        {static_cast<uint16_t>(qol::contracts::kExtensionBagVersionMajor),
         qol::contracts::kExtensionBagPublishedVersionMinor},
        extension_bag_api_table(),
    },
    {
        qol::contracts::kInventoryMergeProjectionCapabilityId,
        {qol::contracts::kInventoryMergeProjectionVersionMajor,
         qol::contracts::kInventoryMergeProjectionVersionMinor},
        &kMergeProjectionApi,
    },
};

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
};

const qol::contracts::CapabilityRequirement kOptionalRequires[] = {
    {qol::contracts::kInventoryRefreshCapabilityId,
     {qol::contracts::kInventoryRefreshVersionMajor,
      qol::contracts::kInventoryRefreshVersionMinor}},
    {qol::contracts::kMoveMergeControlCapabilityId,
     {qol::contracts::kMoveMergeControlVersionMajor,
      qol::contracts::kMoveMergeControlVersionMinor}},
    {qol::contracts::kHttpRegistryCapabilityId,
     {qol::contracts::kHttpRegistryVersion.major, qol::contracts::kHttpRegistryVersion.minor}},
    {qol::contracts::kHttpDispatchCapabilityId,
     {qol::contracts::kHttpDispatchVersion.major, qol::contracts::kHttpDispatchVersion.minor}},
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
const qol::contracts::QolSlotDataApi* g_slot_data_api_cache = nullptr;

void unregister_extension_bag_result_handler(const qol::contracts::QolHostSdk* sdk) {
    if (!g_result_handler_registered || sdk == nullptr ||
        sdk->abi.size < sizeof(qol::contracts::QolHostSdk) || sdk->abi.minor < 8 ||
        sdk->ticket_result.unregister_typed_result_handler == nullptr) {
        return;
    }
    if (contracts::succeeded(
            sdk->ticket_result.unregister_typed_result_handler(kEbResultHandlerId))) {
        g_result_handler_registered = false;
    }
}

void close_extension_bag_gates() {
    g_provider_active.store(false, std::memory_order_release);
    extension_bag_set_enabled(false);
}

// Provider and participant tables are set-once resident registrations. On install failure only
// revoke registrations that can be undone, then keep all resident callbacks gated off.
void rollback_extension_bag_install(const qol::contracts::QolSlotDataApi* slot_data_api = nullptr) {
    close_extension_bag_gates();
    if (slot_data_api == nullptr) slot_data_api = g_slot_data_api_cache;
    if (slot_data_api != nullptr) (void)extension_bag_slot_data_unregister(slot_data_api);
    g_slot_data_api_cache = nullptr;
    unregister_extension_bag_result_handler(g_sdk);
    extension_bag_bind_port_sdk(nullptr);
    extension_bag_bind_item_facts(nullptr);
    extension_bag_bind_craft_facts(nullptr);
    extension_bag_bind_character_extras(nullptr);
    extension_bag_bind_inventory_extras(nullptr);
    g_world_facts = nullptr;
    g_item_facts = nullptr;
    g_save_facts = nullptr;
    g_craft_facts = nullptr;
    g_sdk = nullptr;
    g_game = nullptr;
    g_write_port = nullptr;
}

int32_t slot_data_flush_frame(int64_t, void*) {
    extension_bag_flush_dirty_staging(g_slot_data_api_cache);
    return 1;
}

bool register_extension_bag_result_handler(const qol::contracts::QolHostSdk* sdk) {
    if (sdk == nullptr) return false;
    // 结果处理器需要 host 尾部的票据结果面（QolHostSdk 尾部追加，按 abi 协商）。
    if (sdk->abi.size < sizeof(qol::contracts::QolHostSdk) || sdk->abi.minor < 8 ||
        sdk->ticket_result.register_typed_result_handler == nullptr) {
        return false;
    }
    if (!contracts::succeeded(sdk->ticket_result.register_typed_result_handler(
            kEbResultHandlerId, &eb_result_handler, nullptr))) {
        return false;
    }
    g_result_handler_registered = true;
    return true;
}

int plugin_install(qol::contracts::PluginContext& ctx) {
    if (g_result_handler_registered || g_slot_data_api_cache != nullptr) {
        rollback_extension_bag_install();
    } else {
        close_extension_bag_gates();
    }
    g_sdk = ctx.sdk;
    g_game = ctx.game;
    g_write_port = nullptr;
    extension_bag_bind_character_extras(nullptr);
    extension_bag_bind_inventory_extras(nullptr);
    g_world_facts = nullptr;
    g_item_facts = nullptr;
    g_save_facts = nullptr;
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
    g_world_facts = static_cast<const qol::contracts::WorldFactsApi*>(world_facts_iface);
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
        g_world_facts = nullptr;
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
        g_world_facts = nullptr;
        g_item_facts = nullptr;
        g_save_facts = nullptr;
        extension_bag_bind_port_sdk(nullptr);
        extension_bag_bind_item_facts(nullptr);
        extension_bag_bind_craft_facts(nullptr);
        return 1;
    }
    g_save_facts = static_cast<const qol::contracts::SaveFactsApi*>(save_facts_iface);
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
        g_world_facts = nullptr;
        g_item_facts = nullptr;
        g_save_facts = nullptr;
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
    // The separate move_merge plugin owns the X1 resource.
    // Owner command result handler 注册在全部绑定就绪后执行；失败即 install 失败。
    if (!register_extension_bag_result_handler(g_sdk)) {
        rollback_extension_bag_install();
        return 1;
    }
    // ITM-13：向宿主注册中立物品域 provider（set-once 静态驻留）。注册失败即 install 失败，
    // 回滚已建立的绑定，避免半注册状态。
    if (!qol::host::host_inventory_extension_set_provider(inventory_extension_provider_table(),
                                                          &inventory_extension_provider_active)) {
        rollback_extension_bag_install();
        return 1;
    }
    // 生命周期参与者最末注册（与 provider 是两次独立注册）。注册表 set-once，provider 已
    // 无法撤销；此处失败时保持不可用（active=false）并登记部分注册状态，install 返回失败。
    if (g_sdk == nullptr || !detail::register_lifecycle_participant_once(
                                g_sdk->lifecycle_participant_register, &kLifecycleParticipant,
                                g_lifecycle_participant_registered)) {
        if (g_sdk != nullptr && g_sdk->log.write != nullptr) {
            constexpr char kMessage[] =
                "extension bag lifecycle participant registration failed; provider resident but unavailable";
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
    // 结果处理器同样需要显式注销（同一自注册理由）。
    unregister_extension_bag_result_handler(g_sdk);
    (void)ctx;
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
    // Resources are returned by the host; this callback only closes feature gates.
    // 中立 provider 表静态驻留（不指向可释放内存）；deactivate 只撤销 active，使路由等同无 provider。
    g_provider_active.store(false, std::memory_order_release);
    extension_bag_set_enabled(false);
    g_world_facts = nullptr;
    g_item_facts = nullptr;
    g_save_facts = nullptr;
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
    g_provider_active.store(true, std::memory_order_release);
    extension_bag_set_enabled(true);
}

void plugin_on_disabled(qol::contracts::PluginContext& ctx) {
    // 停用但保留注册句柄：注销 slot_data（先关准入；磁盘 section 保留，不隐式保存）。
    if (g_slot_data_api_cache != nullptr) {
        (void)extension_bag_slot_data_unregister(g_slot_data_api_cache);
    }
    (void)ctx;
    g_provider_active.store(false, std::memory_order_release);
    extension_bag_set_enabled(false);
}

void plugin_fill_status(qol::contracts::PluginStatusOut& out) {
    out.enabled = qol::features::extension_bag::extension_bag_enabled();
    out.error = nullptr;
}

int32_t plugin_on_resource_ready(qol::contracts::PluginContext& ctx, const char* resource_id,
                                 void* backup) {
    (void)ctx;
    // 受限回调（§3.3）：只缓存入口 backup（常数时间），无资源操作。
    return extension_bag_bind_backup(resource_id, backup);
}

// 单一 feature：默认 true（保持旧默认启用行为），用户可见开关。插件仅在 feature
// 开启（want_run）时安装；资源 gate=nullptr 跟随 lifecycle。
QolResult extension_bag_toggle(contracts::PluginContext&, uint8_t enabled) {
    extension_bag_set_enabled(enabled != 0);
    return contracts::ok();
}

constexpr qol::contracts::FeatureDefinition kFeatures[] = {
    {"extension_bag.extended_bag", 1, qol::contracts::FeatureCategory::kGameplay,
      qol::contracts::FeatureType::kToggle, "扩展背包", "增加五个扩展背包。",
      &extension_bag_toggle, nullptr, nullptr, -2},
};

const qol::contracts::PluginDescriptor kDescriptor = {
    kPluginId,
    kPluginVersion,
    5, kRequires,  // requires=world.facts + item.facts + save.facts + craft.facts + save.slot_data
    static_cast<uint32_t>(sizeof(kOptionalRequires) / sizeof(kOptionalRequires[0])),
    kOptionalRequires,
    2, kProvides,  // provides (extension_bag + inventory.merge_projection)
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
    extension_bag_http_endpoints(),
    kHttpEndpointCount,
    nullptr,
    nullptr, 0,
};

}  // namespace

const qol::contracts::QolHostSdk* extension_bag_http_host_sdk() { return g_sdk; }

const qol::contracts::PluginDescriptor& plugin_descriptor() {
    return kDescriptor;
}

const qol::contracts::WorldFactsApi* world_facts() {
    return g_world_facts;
}

const qol::contracts::SaveFactsApi* save_facts() { return g_save_facts; }

bool inventory_extension_provider_active() {
    return g_provider_active.load(std::memory_order_acquire);
}

}  // namespace qol::features::extension_bag
