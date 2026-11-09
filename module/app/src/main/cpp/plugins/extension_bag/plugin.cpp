#include "plugins/extension_bag/plugin.h"

#include <atomic>
#include <cstdint>
#include <cstring>
#include <mutex>

#include "contracts/bag.h"
#include "contracts/craft_facts.h"
#include "contracts/inventory_projection.h"
#include "contracts/ui_confirmation.h"
#include "contracts/inventory_refresh.h"
#include "contracts/inventory_action.h"
#include "contracts/game_ports/move_merge_port.h"
#include "contracts/runtime_hooks.h"
#include "contracts/slot_data.h"
#include "contracts/ui.h"
#include "contracts/world_facts.h"
#include "contracts/item_facts.h"
#include "contracts/save_facts.h"
#include "plugins/extension_bag/api/bag_source.h"
#include "plugins/extension_bag/core/lifecycle.h"
#include "plugins/extension_bag/core/move_merge_hook.h"
#include "plugins/extension_bag/assembly/lifecycle_registration.h"
#include "plugins/extension_bag/assembly/projection_registration.h"
#include "plugins/extension_bag/extension_bag_bridge.h"
#include "plugins/extension_bag/game_ui_virtbag.h"
#include "plugins/extension_bag/ui/equip_hooks.h"
#include "plugins/extension_bag/runtime/inventory_access.h"
#include "plugins/extension_bag/runtime/ownership_port.h"
#include "plugins/extension_bag/runtime/session.h"
#include "plugins/extension_bag/usecases/set_bag_count.h"
#include "plugins/extension_bag/model/bag_catalog.h"
#include "plugins/extension_bag/model/bag_count_setting.h"

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
constexpr const char* kBagCountConfigKey = "bagCount";

constexpr contracts::FeatureChoiceOption kBagCountOptions[] = {
    {"5", "5"}, {"6", "6"}, {"7", "7"}, {"8", "8"},
    {"9", "9"}, {"10", "10"}, {"11", "11"}, {"12", "12"},
    {"13", "13"}, {"14", "14"}, {"15", "15"}, {"16", "16"},
    {"17", "17"}, {"18", "18"}, {"19", "19"}, {"20", "20"},
};

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
// 唯一对外能力是 inventory_refresh（provider 自 core.item 迁入）。

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

// inventory_refresh provider（自 core.item 迁入）：call_original 直调本插件
// UIEquip_RefreshItemArea 的 backup trampoline，消费者是 core.item 的出售结算等路径。
const qol::contracts::CapabilityProvisionRef kProvides[] = {
    {qol::contracts::kInventoryRefreshCapabilityId,
     {qol::contracts::kInventoryRefreshVersionMajor,
      qol::contracts::kInventoryRefreshVersionMinor},
     &kInventoryRefreshApi},
};

const qol::contracts::CapabilityRequirement kOptionalRequires[] = {
    // 页签锚定：句柄存活/身份校验/失效重建由 core.custom_ui 的 ui_components 提供。
    {qol::contracts::kUiComponentsCapabilityId,
     {qol::contracts::kUiContractMajor, qol::contracts::kUiContractMinor}},
    // 可选：出售确认框优先使用带价格文本的共享确认框。
    {qol::contracts::kUiConfirmationCapabilityId,
     {static_cast<uint16_t>(qol::contracts::kUiConfirmationVersionMajor),
      static_cast<uint16_t>(qol::contracts::kUiConfirmationVersionMinor)}},
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
    // 拖拽合并端口（自 core.item 迁入）：宿主白名单允许 core.item 或 extension_bag。
    {qol::contracts::QolPortId::QOL_PORT_MOVE_MERGE,
     {qol::contracts::kQolMoveMergePortVersion.major,
      qol::contracts::kQolMoveMergePortVersion.minor}},
};

// P3c：SlotBound dirty 的锁外安全点排空（RenderPre 每帧）；save.slot_data 的 iface
// 在 install 时 require 后缓存（capability 与插件同生命周期，宿主 revoke 时先于本
// 插件停用 provider——表指针由宿主 capability 表持有，此处不管理其生命周期）。
uint64_t g_slot_data_flush_task = 0;
uint64_t g_deferred_free_task = 0;
uint64_t g_config_apply_task = 0;
std::mutex g_config_mutex;
int32_t g_queued_bag_count = 5;
bool g_bag_count_choice_enabled = false;
uint64_t g_pending_config_write = 0;
qol::contracts::QolOwnerToken g_pending_config_owner{};
const qol::contracts::QolSlotDataApi* g_slot_data_api_cache = nullptr;

void close_extension_bag_gates() {
    g_source_active.store(false, std::memory_order_release);
    extension_bag_bag_source_unregister();
    extension_bag_set_enabled(false);
    merge::set_enabled(false);
}

void remove_config_apply_task() {
    const uint64_t task = g_config_apply_task;
    g_config_apply_task = 0;
    if (task != 0 && g_sdk != nullptr && g_sdk->scheduler.remove != nullptr)
        (void)g_sdk->scheduler.remove(task);
}

// Resident callbacks are gated off on install failure; only reversible registrations
// (slot_data / bag source) are revoked.
void rollback_extension_bag_install(const qol::contracts::QolSlotDataApi* slot_data_api = nullptr) {
    remove_config_apply_task();
    close_extension_bag_gates();
    if (slot_data_api == nullptr) slot_data_api = g_slot_data_api_cache;
    if (slot_data_api != nullptr) (void)extension_bag_slot_data_unregister(slot_data_api);
    g_slot_data_api_cache = nullptr;
    extension_bag_bind_port_sdk(nullptr);
    extension_bag_bind_item_facts(nullptr);
    extension_bag_bind_craft_facts(nullptr);
    extension_bag_bind_character_extras(nullptr);
    extension_bag_bind_inventory_extras(nullptr);
    extension_bag_bind_move_merge(nullptr);
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

void queue_bag_count(int32_t count) {
    std::lock_guard<std::mutex> lock(g_config_mutex);
    g_queued_bag_count = count == 0 ? 5 : count;
    g_bag_count_choice_enabled = count != 0;
}

bool parse_count_string(const char* value, uint32_t size, int32_t minimum,
                        int32_t maximum, int32_t* out) {
    return bag_count_setting::parse_integer(value, size, minimum, maximum, out);
}

bool write_bag_count_config(const qol::contracts::PluginContext& ctx, int32_t count) {
    const auto* api = contracts::feature_if_available(ctx.sdk);
    if (api == nullptr || api->request_set_config == nullptr) {
    if (g_sdk != nullptr && g_sdk->log.write != nullptr) {
            constexpr char message[] = "bagCount config write unavailable";
            (void)g_sdk->log.write(2, 1, kPluginId, message,
                                   static_cast<uint32_t>(sizeof(message) - 1));
        }
        return false;
    }
    char json[16]{};
    const int length = std::snprintf(json, sizeof(json), "%d", count);
    if (length <= 0 || static_cast<size_t>(length) >= sizeof(json)) return false;
    contracts::FeatureReceipt receipt{};
    const QolResult result = api->request_set_config(api->context, ctx.owner_token,
        kPluginId, kBagCountConfigKey, json, static_cast<uint32_t>(length), &receipt);
    if (!contracts::succeeded(result) || receipt.admission != contracts::FeatureAdmission::kAdmitted ||
        receipt.request_id == 0) {
        if (g_sdk != nullptr && g_sdk->log.write != nullptr) {
            constexpr char message[] = "bagCount config write failed; runtime choice retained";
            (void)g_sdk->log.write(2, 1, kPluginId, message,
                                   static_cast<uint32_t>(sizeof(message) - 1));
        }
        return false;
    }
    {
        std::lock_guard<std::mutex> lock(g_config_mutex);
        g_pending_config_write = receipt.request_id;
        g_pending_config_owner = ctx.owner_token;
    }
    return true;
}

void poll_bag_count_config_write() {
    uint64_t request_id = 0;
    qol::contracts::QolOwnerToken owner{};
    {
        std::lock_guard<std::mutex> lock(g_config_mutex);
        request_id = g_pending_config_write;
        owner = g_pending_config_owner;
    }
    if (request_id == 0) return;
    const auto* api = contracts::feature_if_available(g_sdk);
    if (api == nullptr || api->receipt == nullptr) return;
    contracts::FeatureReceipt receipt{};
    const QolResult result = api->receipt(api->context, owner, request_id, &receipt);
    if (!contracts::succeeded(result)) {
        if (g_sdk != nullptr && g_sdk->log.write != nullptr) {
            constexpr char message[] = "bagCount config receipt unavailable";
            (void)g_sdk->log.write(2, 1, kPluginId, message,
                                   static_cast<uint32_t>(sizeof(message) - 1));
        }
    } else if (receipt.persistence == contracts::FeaturePersistence::kPending) {
        return;
    } else if (receipt.persistence == contracts::FeaturePersistence::kFailed ||
               receipt.admission != contracts::FeatureAdmission::kAdmitted) {
        if (g_sdk != nullptr && g_sdk->log.write != nullptr) {
            constexpr char message[] = "bagCount config persistence failed; runtime value retained";
            (void)g_sdk->log.write(2, 1, kPluginId, message,
                                   static_cast<uint32_t>(sizeof(message) - 1));
        }
    }
    std::lock_guard<std::mutex> lock(g_config_mutex);
    if (g_pending_config_write == request_id) {
        g_pending_config_write = 0;
        g_pending_config_owner = {};
    }
}

bag_count_setting::Value read_choice_fallback(const qol::contracts::PluginContext& ctx) {
    const auto* api = contracts::feature_choice_if_available(ctx.sdk);
    if (api == nullptr || api->read_choice == nullptr) {
        return {};
    }
    char selected[32]{};
    uint32_t size = 0;
    const QolResult result = api->read_choice(api->context, ctx.owner_token, kPluginId,
        "extension_bag.bagCount", selected, sizeof(selected), &size);
    if (!contracts::succeeded(result) || size > sizeof(selected)) {
        return {};
    }
    int32_t count = 0;
    if (!parse_count_string(selected, size, 5, 20, &count)) return {};
    return bag_count_setting::from_count(count);
}

void read_initial_bag_count(const qol::contracts::PluginContext& ctx) {
    const auto* api = contracts::feature_if_available(ctx.sdk);
    auto read_state = bag_count_setting::ConfigRead::kNotFound;
    char json[32]{};
    uint32_t size = 0;
    if (api != nullptr && api->read_config != nullptr) {
        const QolResult result = api->read_config(api->context, ctx.owner_token, kPluginId,
            kBagCountConfigKey, json, sizeof(json), &size);
        if (contracts::succeeded(result)) read_state = bag_count_setting::ConfigRead::kFound;
        else if (result.code != kQolErrNotFound) read_state = bag_count_setting::ConfigRead::kFailed;
    }
    auto fallback = bag_count_setting::Value{};
    if (read_state == bag_count_setting::ConfigRead::kNotFound)
        fallback = read_choice_fallback(ctx);
    char fallback_raw[16]{};
    uint32_t fallback_size = 0;
    if (fallback.enabled) {
        const int written = std::snprintf(fallback_raw, sizeof(fallback_raw), "%d", fallback.count);
        if (written > 0 && static_cast<size_t>(written) < sizeof(fallback_raw))
            fallback_size = static_cast<uint32_t>(written);
    }
    const auto setting = bag_count_setting::resolve_startup(read_state,
        read_state == bag_count_setting::ConfigRead::kFound ? json : nullptr,
        read_state == bag_count_setting::ConfigRead::kFound ? size : 0,
        fallback_size != 0, fallback_size != 0 ? fallback_raw : nullptr, fallback_size);
    queue_bag_count(setting.enabled ? setting.count : 0);
    if (!setting.enabled && read_state != bag_count_setting::ConfigRead::kNotFound &&
        g_sdk != nullptr && g_sdk->log.write != nullptr) {
        constexpr char message[] = "bagCount config invalid or unreadable; extension bag disabled";
        (void)g_sdk->log.write(2, 1, kPluginId, message,
                               static_cast<uint32_t>(sizeof(message) - 1));
    }
}

int32_t apply_config_frame(int64_t, void*) {
    poll_bag_count_config_write();
    if (!g_source_active.load(std::memory_order_acquire)) return 1;
    int32_t desired = 5;
    bool enabled = false;
    { std::lock_guard<std::mutex> lock(g_config_mutex);
      desired = g_queued_bag_count; enabled = g_bag_count_choice_enabled; }
    if (enabled) (void)usecases::usecase_set_bag_count(desired);
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

bool bag_count_choice_enabled() {
    std::lock_guard<std::mutex> lock(g_config_mutex);
    return g_bag_count_choice_enabled;
}

int plugin_install(qol::contracts::PluginContext& ctx) {
    if (extension_bag_bag_source_registered() || g_slot_data_api_cache != nullptr) {
        rollback_extension_bag_install();
    } else {
        close_extension_bag_gates();
    }
    g_sdk = ctx.sdk;
    g_game = ctx.game;
    read_initial_bag_count(ctx);
    g_write_port = nullptr;
    extension_bag_bind_character_extras(nullptr);
    extension_bag_bind_inventory_extras(nullptr);
    extension_bag_bind_move_merge(nullptr);
    extension_bag_bind_world_facts(nullptr);
    g_item_facts = nullptr;
    extension_bag_bind_save_facts(nullptr);
    g_craft_facts = nullptr;
    extension_bag_bind_character_extras(nullptr);
    extension_bag_bind_inventory_extras(nullptr);
    extension_bag_bind_move_merge(nullptr);
    if (ctx.struct_size < sizeof(qol::contracts::PluginContext) ||
        ctx.sdk_major != qol::contracts::kHostSdkMajor ||
        ctx.sdk == nullptr || ctx.game == nullptr) {
        g_sdk = nullptr;
        g_game = nullptr;
        g_write_port = nullptr;
    extension_bag_bind_character_extras(nullptr);
    extension_bag_bind_inventory_extras(nullptr);
    extension_bag_bind_move_merge(nullptr);
        extension_bag_bind_character_extras(nullptr);
        extension_bag_bind_inventory_extras(nullptr);
        extension_bag_bind_move_merge(nullptr);
        extension_bag_bind_port_sdk(nullptr);
        extension_bag_bind_item_facts(nullptr);
        extension_bag_bind_craft_facts(nullptr);
        return 1;
    }
    if (ctx.ports == nullptr || ctx.port_count != 4 || ctx.ports[0] == nullptr ||
        ctx.ports[1] == nullptr || ctx.ports[2] == nullptr || ctx.ports[3] == nullptr) {
        g_sdk = nullptr;
        g_game = nullptr;
        g_write_port = nullptr;
    extension_bag_bind_character_extras(nullptr);
    extension_bag_bind_inventory_extras(nullptr);
    extension_bag_bind_move_merge(nullptr);
        extension_bag_bind_character_extras(nullptr);
        extension_bag_bind_inventory_extras(nullptr);
        extension_bag_bind_move_merge(nullptr);
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
    // 拖拽合并端口（QOL_PORT_MOVE_MERGE，自 core.item 迁入）：ABI 与必需函数缺失
    // 时拒绝安装（fail-closed），不保留半可用合并面。
    const auto* move_merge_port =
        static_cast<const qol::contracts::QolMoveMergePort*>(ctx.ports[3]);
    if (!contracts::satisfies(move_merge_port->abi_version,
                              {contracts::kQolMoveMergePortVersion.major,
                               contracts::kQolMoveMergePortVersion.minor}) ||
        move_merge_port->move_item == nullptr || move_merge_port->current_bag == nullptr ||
        move_merge_port->validate_native_item == nullptr) {
        extension_bag_bind_move_merge(nullptr);
        g_sdk = nullptr;
        g_game = nullptr;
        g_write_port = nullptr;
        extension_bag_bind_character_extras(nullptr);
        extension_bag_bind_inventory_extras(nullptr);
        extension_bag_bind_move_merge(nullptr);
        extension_bag_bind_port_sdk(nullptr);
        extension_bag_bind_item_facts(nullptr);
        extension_bag_bind_craft_facts(nullptr);
        return 1;
    }
    extension_bag_bind_move_merge(move_merge_port);
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
    extension_bag_bind_move_merge(nullptr);
        extension_bag_bind_character_extras(nullptr);
        extension_bag_bind_inventory_extras(nullptr);
        extension_bag_bind_move_merge(nullptr);
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
    extension_bag_bind_move_merge(nullptr);
        extension_bag_bind_character_extras(nullptr);
        extension_bag_bind_inventory_extras(nullptr);
        extension_bag_bind_move_merge(nullptr);
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
    extension_bag_bind_move_merge(nullptr);
        extension_bag_bind_character_extras(nullptr);
        extension_bag_bind_inventory_extras(nullptr);
        extension_bag_bind_move_merge(nullptr);
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
    extension_bag_bind_move_merge(nullptr);
        extension_bag_bind_character_extras(nullptr);
        extension_bag_bind_inventory_extras(nullptr);
        extension_bag_bind_move_merge(nullptr);
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
    extension_bag_bind_move_merge(nullptr);
        extension_bag_bind_character_extras(nullptr);
        extension_bag_bind_inventory_extras(nullptr);
        extension_bag_bind_move_merge(nullptr);
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
    if (g_sdk->scheduler.add != nullptr && g_config_apply_task == 0) {
        uint64_t task = 0;
        if (contracts::succeeded(g_sdk->scheduler.add(qol::contracts::kQolRuntimeFramePointLogicPre,
                &apply_config_frame, nullptr, 1, 0, &task))) g_config_apply_task = task;
    }
    // 等价原 core.item merge::attach 成功置位：安装成功即允许合并（wrapper 就绪态
    // 由 on_resource_ready 单独维护，未就绪前门禁仍透传）。
    merge::set_enabled(true);
    return 0;
}

void plugin_deactivate(qol::contracts::PluginContext& ctx) {
    (void)ctx;
    // 先注销袋源与投影回调：注销后 core.item 不再回调本插件，随后才拆绑定。
    extension_bag_bag_source_unregister();
    unregister_projection_callbacks();
    // merge ptrslot 迁入后：deactivate 即资源归还宿主，wrapper 就绪态一并清零。
    merge::detach();
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
    remove_config_apply_task();
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
    extension_bag_bind_move_merge(nullptr);
    g_sdk = nullptr;
}

// 启停仅切门禁，资源安装/还原由宿主完成（§3.3）。
void plugin_on_enabled(qol::contracts::PluginContext& ctx) {
    close_extension_bag_gates();
    read_initial_bag_count(ctx);
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
    if (!bag_count_choice_enabled()) {
        extension_bag_bag_source_unregister();
        unregister_projection_callbacks();
        g_source_active.store(false, std::memory_order_release);
        extension_bag_set_enabled(false);
        merge::set_enabled(true);
        return;
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
    // 等价原 core.item merge::attach 的置位语义：插件进入 enabled 即允许合并，
    // ptrslot 就绪态由 on_resource_ready 单独维护。
    merge::set_enabled(true);
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
    merge::set_enabled(false);
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
    // merge ptrslot（自 core.item 迁入）：就绪即开 wrapper 门禁；无 backup 需缓存。
    if (merge::is_merge_resource(resource_id)) {
        merge::set_wrapper_ready(true);
        return 0;
    }
    // 受限回调（§3.3）：只缓存入口 backup（常数时间），无资源操作。
    return extension_bag_bind_backup(resource_id, backup);
}

// Features：背包数量 choice 取代旧主开关；拖拽合并独立保留。
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

QolResult extension_bag_bag_count_choice(contracts::PluginContext& ctx, const char* value) {
    int32_t count = 0;
    if (value != nullptr && value[0] != '\0') {
        if (!parse_count_string(value, static_cast<uint32_t>(std::strlen(value)), 5, 20, &count))
            return contracts::error(kQolErrInvalidArgument, "bagCount choice must be 5..20", 29);
    }
    return bag_count_setting::apply_and_persist_nonblocking(count,
        [&](int32_t chosen) {
            queue_bag_count(chosen);
            return extension_bag_toggle(ctx, chosen == 0 ? 0 : 1);
        },
        [&](int32_t chosen) { return write_bag_count_config(ctx, chosen); });
}

constexpr qol::contracts::FeatureDefinition kFeatures[] = {
    {"extension_bag.bagCount", 0, qol::contracts::FeatureCategory::kGameplay,
     qol::contracts::FeatureType::kChoice, "额外背包数量",
     "扩展背包可用袋数量（页签最多显示 20 个）", nullptr, nullptr, nullptr, -2,
     kBagCountOptions, static_cast<uint32_t>(std::size(kBagCountOptions)),
     &extension_bag_bag_count_choice},
    {"move_merge.drag_merge", 1, qol::contracts::FeatureCategory::kBasicOptimization,
     qol::contracts::FeatureType::kToggle, "拖动合并", "允许合并同种可堆叠物品",
     [](contracts::PluginContext&, uint8_t enabled) -> QolResult {
         merge::set_enabled(enabled != 0);
         return contracts::ok();
     },
     nullptr},
};

const qol::contracts::PluginDescriptor kDescriptor = {
    kPluginId,
    kPluginVersion,
    8, kRequires,  // requires=world.facts + item.facts + save.facts + craft.facts + save.slot_data + inventory.bag_source + inventory.action 3.0 + inventory.projection 1.0
    static_cast<uint32_t>(sizeof(kOptionalRequires) / sizeof(kOptionalRequires[0])),
    kOptionalRequires,
    static_cast<uint32_t>(sizeof(kProvides) / sizeof(kProvides[0])),
    kProvides,
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
    4,  // extension_bag_write + character_extras + inventory_extras + move_merge
    nullptr, 0,
    kFeatures, 2,
    nullptr,
    0,
    &bag_count_setting::validate_config,
    nullptr, 0,
};

}  // namespace

int extension_bag_configured_bag_count() {
    std::lock_guard<std::mutex> lock(g_config_mutex);
    return g_queued_bag_count;
}

const qol::contracts::PluginDescriptor& plugin_descriptor() {
    return kDescriptor;
}

}  // namespace qol::features::extension_bag
