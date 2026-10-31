#include "plugin.h"

#include <cstdarg>
#include <cstdio>
#include <cstdint>

#include "api/access.h"
#include "api/bag_source.h"
#include "hooks/storage_hooks.h"
#include "runtime/context.h"

#include "contracts/game_ports/item_port.h"

namespace qol::features::core_inventory {
namespace {

constexpr const char* kPluginId = "core.inventory";
constexpr const char* kPluginVersion = "0.1.0";
constexpr int32_t kLogDomain = 5;  // QolDomain::kInventory

const contracts::QolHostSdk* g_sdk = nullptr;
const contracts::QolItemPort* g_port = nullptr;
bool g_installed = false;
bool g_enabled = false;

// 物理扫描上界 = 原版袋数 + C18 探测到的追加袋数，最终夹到 item port 布局容量。
void apply_native_bag_limit() {
    if (g_port == nullptr || g_port->read.bag_count == nullptr ||
        !contracts::qol_item_port_bag_space_size_valid(g_port)) {
        return;
    }
    int32_t limit = contracts::kQolOriginalBagCount;
    int32_t original = 0;
    if (contracts::succeeded(
            g_port->read.bag_count(contracts::kQolBagSpaceOriginal, &original)) &&
        original > 0) {
        limit = original;
    }
    int32_t c18 = 0;
    if (contracts::succeeded(g_port->read.bag_count(contracts::kQolBagSpaceVariant, &c18)) &&
        c18 > 0) {
        limit += c18;
    }
    bags::set_native_bag_limit(limit);
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

int plugin_install(contracts::PluginContext& context) {
    if (context.sdk == nullptr || context.game == nullptr || context.ports == nullptr ||
        context.port_count < 1 || context.ports[0] == nullptr) {
        return 1;
    }
    const auto* port = static_cast<const contracts::QolItemPort*>(context.ports[0]);
    if (!contracts::satisfies(port->abi_version, {contracts::kQolPortItemVersion.major,
                                                 contracts::kQolPortItemVersion.minor}) ||
        !contracts::qol_item_port_bag_space_size_valid(port)) {
        return 1;
    }
    g_sdk = context.sdk;
    g_port = port;
    g_installed = true;
    inventory_store().clear();
    reset_bag_sources();
    if (!hooks::attach(context.sdk, context.game, port)) {
        g_sdk = nullptr;
        g_port = nullptr;
        g_installed = false;
        return 1;
    }
    apply_native_bag_limit();
    log_line(1,
             "plugin=%s: installed version=%s mode=storage "
             "provides=inventory.access,inventory.bag_source resources=12",
             kPluginId, kPluginVersion);
    return 0;
}

void plugin_deactivate(contracts::PluginContext&) {
    log_line(1, "plugin=%s: deactivated", kPluginId);
    hooks::detach();
    inventory_store().clear();
    reset_bag_sources();
    g_sdk = nullptr;
    g_port = nullptr;
    g_installed = false;
    g_enabled = false;
}

void plugin_on_enabled(contracts::PluginContext&) {
    g_enabled = true;
    hooks::set_support(true);
    apply_native_bag_limit();
    log_line(1, "plugin=%s: enabled", kPluginId);
}

void plugin_on_disabled(contracts::PluginContext&) {
    g_enabled = false;
    hooks::set_support(false);
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
};

const contracts::QolPortRequirement kPorts[] = {
    {contracts::QolPortId::QOL_PORT_ITEM,
     {contracts::kQolPortItemVersion.major, contracts::kQolPortItemVersion.minor}},
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
    0,
    nullptr,
    2,
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
    1,
    nullptr,
    0,
    kFeatures,
    1,
};

}  // namespace

const contracts::PluginDescriptor& plugin_descriptor() { return kDescriptor; }

}  // namespace qol::features::core_inventory
