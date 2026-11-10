#include "plugin.h"

#include <cstdarg>
#include <cstdio>
#include <cstdint>

#include "api/access.h"
#include "api/bag_source.h"
#include "bags/snapshot_store.h"
#include "hooks/storage_hooks.h"
#include "http/routes.h"
#include "http/write_handler.h"
#include "http/write_results.h"
#include "runtime/context.h"
#include "transfer/api.h"
#include "usecases/read_inventory.h"

#include "contracts/game_ports/item_inventory_state_write_port.h"
#include "contracts/game_ports/item_money_write_port.h"
#include "contracts/game_ports/item_port.h"
#include "contracts/inventory_transfer.h"

#if !defined(QOL_CORE_INVENTORY_TRANSFER_SEPARATE)
// 宿主测试直接编译 plugin.cpp，不读取插件 CMake 源清单；插件 target 单独编译该实现。
#include "plugins/core.inventory/transfer/api.cpp"
#endif
#if !defined(QOL_CORE_INVENTORY_WRITE_SEPARATE)
// 宿主测试直接编译 plugin.cpp，不读取插件 CMake 源清单；插件 target 单独编译写侧实现。
#include "plugins/core.inventory/http/write_handler.cpp"
#include "plugins/core.inventory/http/write_results.cpp"
#endif

namespace qol::features::core_inventory {
namespace {

constexpr const char* kPluginId = "core.inventory";
constexpr const char* kPluginVersion = "0.1.0";
constexpr int32_t kLogDomain = 5;  // QolDomain::kInventory
// 与 core.item 快照兜底任务一致的刷新间隔；HTTP 查询只读已发布快照。
constexpr int32_t kSnapshotIntervalFrames = 60;
uint64_t g_frame_task = 0;

bool g_installed = false;
bool g_enabled = false;
// 尾部票据结果处理器是否已注册：宿主不支持时不注册也不阻断安装（HTTP 写命令投递会失败）。
bool g_write_result_registered = false;

void register_write_result_handler() {
    g_write_result_registered = false;
    if (g_sdk == nullptr || g_sdk->abi.size < sizeof(contracts::QolHostSdk) ||
        g_sdk->abi.minor < 9 ||
        g_sdk->ticket_result.register_typed_result_handler == nullptr) {
        return;
    }
    g_write_result_registered =
        contracts::succeeded(g_sdk->ticket_result.register_typed_result_handler(
            kInventoryWriteHandlerId, &inventory_write_result, nullptr));
}

void unregister_write_result_handler() {
    if (g_sdk != nullptr && g_write_result_registered &&
        g_sdk->ticket_result.unregister_typed_result_handler != nullptr) {
        (void)g_sdk->ticket_result.unregister_typed_result_handler(kInventoryWriteHandlerId);
    }
    g_write_result_registered = false;
}

// 物理扫描上界 = 模块布局事实（原版 6 袋 + C18 探测到的追加袋数）。
void apply_native_bag_limit() {
    if (g_port == nullptr || !contracts::qol_item_port_native_bag_size_valid(g_port)) {
        return;
    }
    contracts::QolNativeBagLayout layout{};
    if (contracts::succeeded(g_port->read.native_bag_layout(&layout)) && layout.bag_limit > 0) {
        bags::set_native_bag_limit(layout.bag_limit);
    }
}

void log_line(int32_t level, const char* format, ...) {
    if (g_sdk == nullptr || g_sdk->log.write == nullptr || format == nullptr) return;
    char message[512]{};
    va_list args;
    va_start(args, format);
    const int written = std::vsnprintf(message, sizeof(message), format, args);
    va_end(args);
    if (written <= 0) return;
    const uint32_t length = static_cast<uint32_t>(written) >= sizeof(message)
                                ? static_cast<uint32_t>(sizeof(message) - 1)
                                : static_cast<uint32_t>(written);
    (void)g_sdk->log.write(level, kLogDomain, kPluginId, message, length);
}

// 快照任务 fail-soft：scheduler 不可用只记日志，不影响存储 hook 与 descriptor 注册。
void stop_frame_task() {
    if (g_frame_task != 0 && g_sdk != nullptr && g_sdk->scheduler.remove != nullptr) {
        (void)g_sdk->scheduler.remove(g_frame_task);
    }
    g_frame_task = 0;
}

void start_frame_task() {
    if (g_frame_task != 0 || g_sdk == nullptr || g_sdk->scheduler.add == nullptr) return;
    uint64_t task = 0;
    if (contracts::succeeded(g_sdk->scheduler.add(
            1, &snapshot_frame_tick, nullptr, kSnapshotIntervalFrames, 0, &task))) {
        g_frame_task = task;
    } else {
        log_line(2, "plugin=%s: inventory snapshot scheduler unavailable", kPluginId);
    }
}

int plugin_install(contracts::PluginContext& context) {
    if (context.sdk == nullptr || context.game == nullptr || context.ports == nullptr ||
        context.port_count != 3 || context.ports[0] == nullptr ||
        context.ports[1] == nullptr || context.ports[2] == nullptr) {
        return 1;
    }
    const auto* port = static_cast<const contracts::QolItemPort*>(context.ports[0]);
    if (!contracts::satisfies(port->abi_version, {contracts::kQolPortItemVersion.major,
                                                  contracts::kQolPortItemVersion.minor}) ||
        !contracts::qol_item_port_native_bag_size_valid(port)) {
        return 1;
    }
    const auto* item_money_write =
        static_cast<const contracts::QolItemMoneyWritePort*>(context.ports[1]);
    if (!contracts::satisfies(item_money_write->abi_version,
                              {contracts::kQolPortItemMoneyWriteVersion.major,
                               contracts::kQolPortItemMoneyWriteVersion.minor}) ||
        item_money_write->set_money == nullptr) {
        return 1;
    }
    const auto* item_state_write =
        static_cast<const contracts::QolItemInventoryStateWritePort*>(context.ports[2]);
    // transfer 只消费 load_item/move_handle/remove_item/move_item 原语；
    // 其余 use/sell/equip 写回调仍归 core.item 使用，这里不要求。
    if (!contracts::satisfies(item_state_write->abi_version,
                              {contracts::kQolPortItemInventoryStateWriteVersion.major,
                               contracts::kQolPortItemInventoryStateWriteVersion.minor}) ||
        item_state_write->load_item == nullptr || item_state_write->move_handle == nullptr ||
        item_state_write->remove_item == nullptr || item_state_write->move_item == nullptr) {
        return 1;
    }
    g_sdk = context.sdk;
    g_port = port;
    g_game = context.game;
    g_item_money_write = item_money_write;
    g_item_state_write = item_state_write;
    g_installed = true;
    inventory_store().clear();
    reset_bag_sources();
    bags::inventory_snapshot_store().clear();
    register_write_result_handler();
    if (!hooks::attach(context.sdk, context.game, port)) {
        unregister_write_result_handler();
        g_sdk = nullptr;
        g_port = nullptr;
        g_game = nullptr;
        g_item_money_write = nullptr;
        g_item_state_write = nullptr;
        g_installed = false;
        return 1;
    }
    apply_native_bag_limit();
    initialize_http_endpoints();
    log_line(1,
             "plugin=%s: installed version=%s mode=storage "
             "provides=inventory.access,inventory.bag_source,inventory.transfer resources=12",
             kPluginId, kPluginVersion);
    return 0;
}

void plugin_deactivate(contracts::PluginContext&) {
    log_line(1, "plugin=%s: deactivated", kPluginId);
    stop_frame_task();
    bags::inventory_snapshot_store().clear();
    hooks::detach();
    inventory_store().clear();
    reset_bag_sources();
    unregister_write_result_handler();
    g_sdk = nullptr;
    g_port = nullptr;
    g_game = nullptr;
    g_item_money_write = nullptr;
    g_item_state_write = nullptr;
    g_installed = false;
    g_enabled = false;
}

void plugin_on_enabled(contracts::PluginContext&) {
    g_enabled = true;
    hooks::set_support(true);
    apply_native_bag_limit();
    bags::inventory_snapshot_store().clear();
    start_frame_task();
    log_line(1, "plugin=%s: enabled", kPluginId);
}

void plugin_on_disabled(contracts::PluginContext&) {
    g_enabled = false;
    hooks::set_support(false);
    stop_frame_task();
    bags::inventory_snapshot_store().clear();
    log_line(1, "plugin=%s: disabled", kPluginId);
}

void plugin_fill_status(contracts::PluginStatusOut& out) {
    out.enabled = g_installed && g_enabled;
    out.error = g_installed ? nullptr : "not installed";
}

QolResult feature_toggle(contracts::PluginContext&, uint8_t enabled) {
    log_line(1, "plugin=%s: feature toggle enabled=%u", kPluginId,
             static_cast<unsigned>(enabled));
    return contracts::ok();
}

const contracts::FeatureDefinition kFeatures[] = {
    {"core.inventory.enabled", 1, contracts::FeatureCategory::kHidden,
     contracts::FeatureType::kToggle, "库存层（骨架）", "存储层占位，暂不提供运行时能力。",
     &feature_toggle, nullptr},
};

const contracts::CapabilityProvisionRef kProvides[] = {
    {contracts::kInventoryAccessCapabilityId, contracts::kInventoryAccessVersion,
     &inventory_access_api()},
    {contracts::kBagSourceCapabilityId, contracts::kBagSourceVersion,
     &kBagSourceRegistryApi},
    {contracts::kInventoryTransferCapabilityId, contracts::kInventoryTransferVersion,
     &transfer::inventory_transfer_api()},
};

// HTTP 注册面缺失时读侧 endpoint 不生效（descriptor 不发布 handler），存储 hook 不受影响。
const contracts::CapabilityRequirement kOptionalRequires[] = {
    {contracts::kHttpRegistryCapabilityId,
     {contracts::kHttpRegistryVersion.major, contracts::kHttpRegistryVersion.minor}},
    {contracts::kHttpDispatchCapabilityId,
     {contracts::kHttpDispatchVersion.major, contracts::kHttpDispatchVersion.minor}},
};

// 端口顺序即 plugin_install 的注入顺序：item、money 写、库存状态写。
const contracts::QolPortRequirement kPorts[] = {
    {contracts::QolPortId::QOL_PORT_ITEM,
     {contracts::kQolPortItemVersion.major, contracts::kQolPortItemVersion.minor}},
    {contracts::QolPortId::QOL_PORT_ITEM_MONEY_WRITE,
     {contracts::kQolPortItemMoneyWriteVersion.major,
      contracts::kQolPortItemMoneyWriteVersion.minor}},
    {contracts::QolPortId::QOL_PORT_ITEM_INVENTORY_STATE_WRITE,
     {contracts::kQolPortItemInventoryStateWriteVersion.major,
      contracts::kQolPortItemInventoryStateWriteVersion.minor}},
};

uint32_t g_resource_count = 0;
const contracts::ResourceClaim* g_resources = hooks::claims(&g_resource_count);

int32_t on_resource_ready(contracts::PluginContext&, const char* resource_id, void* backup) {
    return hooks::bind_backup(resource_id, backup);
}

const contracts::PluginDescriptor kDescriptor = {
    kPluginId,
    kPluginVersion,
    0,
    nullptr,
    static_cast<uint32_t>(sizeof(kOptionalRequires) / sizeof(kOptionalRequires[0])),
    kOptionalRequires,
    static_cast<uint32_t>(sizeof(kProvides) / sizeof(kProvides[0])),
    kProvides,
    g_resource_count,
    g_resources,
    0,
    nullptr,
    &plugin_install,
    &plugin_deactivate,
    &plugin_on_enabled,
    &plugin_on_disabled,
    &plugin_fill_status,
    &on_resource_ready,
    kPorts,
    static_cast<uint32_t>(sizeof(kPorts) / sizeof(kPorts[0])),
    nullptr,
    0,
    kFeatures,
    1,
    kHttpEndpoints,
    kHttpEndpointCount,
    nullptr,
    kHttpRouteMetadata,
    kHttpRouteMetadataCount,
};

}  // namespace

const contracts::PluginDescriptor& plugin_descriptor() {
    initialize_http_endpoints();
    return kDescriptor;
}

}  // namespace qol::features::core_inventory
