#include "plugin.h"

#include <cstdint>

#include "../../contracts/game_ports/craft_lottery_port.h"
#include "feature.h"

namespace qol::features::craft_lottery {
namespace {

constexpr const char* kPluginId = "craft_lottery";
constexpr const char* kPluginVersion = "0.1.0";

const contracts::QolHostSdk* g_sdk = nullptr;
const contracts::QolCraftLotteryWritePort* g_port = nullptr;

const contracts::QolPortRequirement kPorts[] = {
    {contracts::QolPortId::QOL_PORT_CRAFT_LOTTERY_WRITE,
     {contracts::kQolPortCraftLotteryWriteVersion.major,
      contracts::kQolPortCraftLotteryWriteVersion.minor}},
};

const contracts::ResourceClaim kResources[] = {
    {
        kResourceIdProcessRecipe,
        "F_CHAR_PROCESS_RECIPE_VMA",
        contracts::ResourceMechanism::kEntryHook,
        contracts::ResourcePolicy::kPrivate,
        contracts::ResourceRequirement::kRequired,
        contracts::ResourceFailure::kFailClosed,
        true,
        kPluginId,
        "合成套装使用效果唯一入口；wrap 实现保底与弹窗追加",
        nullptr,
        120,
        reinterpret_cast<void*>(&process_recipe_wrapper),
    },
};

void clear_install_state() {
    set_gate(false);
    g_sdk = nullptr;
    g_port = nullptr;
}

bool port_functions_ready(const contracts::QolCraftLotteryWritePort* port) {
    return port != nullptr && port->pool_read != nullptr && port->recipe_learned != nullptr &&
           port->decide_recipe_pure != nullptr && port->add_recipe_book != nullptr &&
           port->get_random != nullptr && port->show_recipe_popup != nullptr;
}

int plugin_install(contracts::PluginContext& ctx) {
    set_gate(false);
    g_sdk = ctx.sdk;
    g_port = nullptr;
    if (ctx.struct_size < sizeof(contracts::PluginContext) ||
        ctx.sdk_major != contracts::kHostSdkMajor || g_sdk == nullptr || ctx.ports == nullptr ||
        ctx.port_count != 1 || ctx.ports[0] == nullptr) {
        clear_install_state();
        return 1;
    }

    const auto* port = static_cast<const contracts::QolCraftLotteryWritePort*>(ctx.ports[0]);
    if (!contracts::satisfies(
            port->abi_version,
            {contracts::kQolPortCraftLotteryWriteVersion.major,
             contracts::kQolPortCraftLotteryWriteVersion.minor}) ||
        !port_functions_ready(port)) {
        clear_install_state();
        return 1;
    }
    g_port = port;
    return 0;
}

void plugin_deactivate(contracts::PluginContext& ctx) {
    (void)ctx;
    clear_install_state();
}

void plugin_on_enabled(contracts::PluginContext& ctx) {
    (void)ctx;
    set_gate(true);
}

void plugin_on_disabled(contracts::PluginContext& ctx) {
    (void)ctx;
    set_gate(false);
}

void plugin_fill_status(contracts::PluginStatusOut& out) {
    out.enabled = gate_enabled();
    out.error = nullptr;
}

constexpr contracts::FeatureDefinition kFeatures[] = {
    {"craft_lottery.guarantee", 0, contracts::FeatureCategory::kBasicOptimization,
     contracts::FeatureType::kToggle, "合成套装保底", "合成套装抽奖未集齐时只抽未拥有的配方",
     [](contracts::PluginContext&, uint8_t enabled) -> QolResult {
         if (enabled == 0) set_gate(false);
         return contracts::ok();
     },
     nullptr, nullptr, -1},
};

const contracts::PluginDescriptor kDescriptor = {
    kPluginId,
    kPluginVersion,
    0, nullptr,
    0, nullptr,
    0, nullptr,
    1, kResources,
    0, nullptr,
    &plugin_install,
    &plugin_deactivate,
    &plugin_on_enabled,
    &plugin_on_disabled,
    &plugin_fill_status,
    &on_resource_ready,
    kPorts,
    1,
    nullptr, 0,
    kFeatures, 1,
};

}  // namespace

const contracts::QolHostSdk* host_sdk() { return g_sdk; }

const contracts::QolCraftLotteryWritePort* craft_lottery_write_port() { return g_port; }

const contracts::PluginDescriptor& plugin_descriptor() { return kDescriptor; }

}  // namespace qol::features::craft_lottery
