#include "plugins/quest/plugin.h"

#include <cstdint>

#include "core/native/save_enter.h"
#include "plugins/quest/feature.h"

namespace qol::features::quest {
namespace {

constexpr const char* kPluginId = "quest";
constexpr const char* kPluginVersion = "0.1.0";

int plugin_install(contracts::PluginContext& ctx) {
    (void)ctx;
    // save_enter 回调设施是 8 个使用者的共享依赖，本轮不迁移；迁移延后（见 ledger 备注）。
    // register 对同一 fn+ctx 幂等，activate 重入安全。
    save_enter_register(&on_save_enter, nullptr);
    set_gate(true);
    return 0;
}

void plugin_deactivate(contracts::PluginContext& ctx) {
    (void)ctx;
    set_gate(false);
    // 注销 legacy 回调：设施迁移前与注册成对，避免停用后仍收到 save_enter。
    save_enter_unregister(&on_save_enter, nullptr);
}

// 零资源、无门禁键：启停只切业务门禁，不做资源操作（§3.3）。
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

const contracts::PluginDescriptor& plugin_descriptor() {
    return kDescriptor;
}

}  // namespace qol::features::quest
