#include "plugins/text_layer/plugin.h"

#include <cstdint>

#include "contracts/text_layer.h"
#include "core/native/qol_log.h"
#include "plugins/text_layer/feature.h"

namespace qol::features::text_layer {
namespace {

constexpr const char* kPluginId = "text_layer";
constexpr const char* kPluginVersion = "0.1.0";

// 能力接口实例：函数地址为编译期常量，无跨 TU 静态初始化顺序问题。
constexpr contracts::TextLayerApi kTextLayerApi{&api_subscribe, &api_unsubscribe};

// MEMORYTEXT_GetText 为**共享资源**（§4.3/§4.5）：多消费者经 hooks/dispatch 接入，
// owner 独占同一地址的 hook；gate=nullptr（常驻）。
const contracts::ResourceClaim kResources[] = {
    {
        kResourceIdGetText,        // id
        "F_MEMORYTEXT_GET_TEXT_VMA",  // symbol
        contracts::ResourceMechanism::kEntryHook,
        contracts::ResourcePolicy::kShared,   // 共享资源：消费者只登记回调
        contracts::ResourceRequirement::kRequired,
        contracts::ResourceFailure::kFailClosed,
        true,                      // resumable
        kPluginId,                 // owner
        "全游戏唯一文本热点（232 调用点）；LSPosed 同址二次挂载失败，只能单一 owner + dispatch 分派",
        nullptr,                   // gate（常驻）
        120,                       // step_budget
        reinterpret_cast<void*>(&memorytext_get_text_wrapper),  // handler
    },
};

const contracts::CapabilityProvisionRef kProvides[] = {
    {
        contracts::kTextLayerCapabilityId,
        {contracts::kTextLayerVersionMajor, contracts::kTextLayerVersionMinor},
        &kTextLayerApi,
    },
};

int plugin_install(contracts::PluginContext& ctx) {
    (void)ctx;
    // 内置表消费者（after+Modify）在 install 注册；失败（容量满等）→ fail-closed，
    // 宿主按 install 失败回滚并把插件置 failed（§3.2 部分安装失败）。
    if (!register_builtin_consumers()) {
        QOL_LOG_ERROR(QolDomain::kUi, "text_layer builtin table consumer registration failed");
        return 1;
    }
    set_active(true);
    return 0;
}

void plugin_deactivate(contracts::PluginContext& ctx) {
    (void)ctx;
    unregister_builtin_consumers();
    set_active(false);
}

void plugin_on_enabled(contracts::PluginContext& ctx) {
    (void)ctx;
    set_active(true);
}

void plugin_on_disabled(contracts::PluginContext& ctx) {
    (void)ctx;
    set_active(false);
}

void plugin_fill_status(contracts::PluginStatusOut& out) {
    out.enabled = active();
    out.error = nullptr;
}

const contracts::PluginDescriptor kDescriptor = {
    kPluginId,
    kPluginVersion,
    0, nullptr,   // requires
    0, nullptr,   // optional_requires
    1, kProvides, // provides（text_layer v1）
    1, kResources,
    0, nullptr,   // jni
    &plugin_install,
    &plugin_deactivate,
    &plugin_on_enabled,
    &plugin_on_disabled,
    &plugin_fill_status,
    &on_resource_ready,
};

}  // namespace

const contracts::PluginDescriptor& plugin_descriptor() {
    return kDescriptor;
}

}  // namespace qol::features::text_layer
