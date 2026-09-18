#include "plugins/quest/plugin.h"

#include <cstdint>

#include "plugins/quest/feature.h"

namespace qol::features::quest {
namespace {

constexpr const char* kPluginId = "quest";
constexpr const char* kPluginVersion = "0.1.0";

const contracts::QolHostSdk* g_sdk = nullptr;
const contracts::QolGameHost* g_game = nullptr;

int plugin_install(contracts::PluginContext& ctx) {
    g_sdk = ctx.sdk;
    g_game = ctx.game;
    if (ctx.struct_size < sizeof(contracts::PluginContext) ||
        ctx.sdk_major != contracts::kHostSdkMajor || g_sdk == nullptr || g_game == nullptr) {
        g_sdk = nullptr;
        g_game = nullptr;
        return 1;
    }
    set_gate(true);
    start_resync_task();
    return 0;
}

void plugin_deactivate(contracts::PluginContext& ctx) {
    (void)ctx;
    set_gate(false);
    cancel_resync_task();
}

// 零资源、无门禁键：启停只切业务门禁，不做资源操作（§3.3）。
void plugin_on_enabled(contracts::PluginContext& ctx) {
    (void)ctx;
    set_gate(true);
    start_resync_task();
}

void plugin_on_disabled(contracts::PluginContext& ctx) {
    (void)ctx;
    set_gate(false);
    cancel_resync_task();
}

void plugin_fill_status(contracts::PluginStatusOut& out) {
    out.enabled = gate_enabled();
    out.error = nullptr;
}

const contracts::PluginDescriptor kDescriptor = {
    kPluginId,
    kPluginVersion,
    0, nullptr,   // requires
    0, nullptr,   // optional_requires
    0, nullptr,   // provides
    0, nullptr,   // resources（零资源）
    0, nullptr,   // jni
    &plugin_install,
    &plugin_deactivate,
    &plugin_on_enabled,
    &plugin_on_disabled,
    &plugin_fill_status,
    nullptr,      // on_resource_ready（无资源）
};

}  // namespace

const contracts::QolHostSdk* host_sdk() { return g_sdk; }

const contracts::QolGameHost* game_host() { return g_game; }

const contracts::PluginDescriptor& plugin_descriptor() {
    return kDescriptor;
}

}  // namespace qol::features::quest
