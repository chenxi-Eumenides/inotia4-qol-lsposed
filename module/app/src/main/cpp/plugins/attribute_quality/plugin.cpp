#include "plugin.h"

#include <cstdint>

#include "../../contracts/attribute_quality.h"
#include "../../contracts/item_facts.h"
#include "feature.h"

namespace qol::features::attribute_quality {
namespace {

constexpr const char* kPluginId = "attribute_quality";
constexpr const char* kPluginVersion = "0.1.0";

constexpr contracts::AttributeQualityApi kAttributeQualityApi{
    &api_probe_jewel, &api_probe_option, &api_classify, &api_percentile};

// MATH_GetRandom 是范围探测的唯一捕获窗口；UIDesc_* 属 display 消费者。
const contracts::ResourceClaim kResources[] = {
    {
        kResourceIdMathGetRandom,
        "F_MATH_GET_RANDOM_VMA",
        contracts::ResourceMechanism::kEntryHook,
        contracts::ResourcePolicy::kPrivate,
        contracts::ResourceRequirement::kRequired,
        contracts::ResourceFailure::kFailClosed,
        true,
        kPluginId,
        "范围探测同步调用游戏随机函数；wrapper 捕获区间并透传原函数",
        nullptr,
        120,
        reinterpret_cast<void*>(&math_get_random_wrapper),
    },
};

// 能力提供声明：id + 版本 + 接口指针（§3.3）。
const contracts::CapabilityProvisionRef kProvides[] = {
    {
        contracts::kAttributeQualityCapabilityId,
        {contracts::kAttributeQualityVersionMajor, contracts::kAttributeQualityVersionMinor},
        &kAttributeQualityApi,
    },
};

const contracts::CapabilityRequirement kRequires[] = {
    {contracts::kItemFactsCapabilityId,
     {contracts::kItemFactsVersionMajor, contracts::kItemFactsVersionMinor}},
};

int plugin_install(contracts::PluginContext& ctx) {
    if (ctx.sdk == nullptr || ctx.sdk->capabilities.require == nullptr) return 1;
    const void* iface = nullptr;
    if (!contracts::succeeded(ctx.sdk->capabilities.require(
            contracts::kItemFactsCapabilityId,
            {contracts::kItemFactsVersionMajor, contracts::kItemFactsVersionMinor}, &iface)) ||
        iface == nullptr || !contracts::item_facts_api_size_valid(
                                static_cast<const contracts::ItemFactsApi*>(iface))) {
        return 1;
    }
    set_item_facts(static_cast<const contracts::ItemFactsApi*>(iface));
    set_context(ctx);
    set_active(true);
    return 0;
}

void plugin_deactivate(contracts::PluginContext& ctx) {
    (void)ctx;
    set_item_facts(nullptr);
    set_active(false);
}

void plugin_on_enabled(contracts::PluginContext& ctx) {
    (void)ctx;
    set_active(true);
}

void plugin_on_disabled(contracts::PluginContext& ctx) {
    (void)ctx;
    // 撤销路径的“关闭提供者门禁”信号：on_disabled 先于级联/注销被宿主调用。
    set_active(false);
}

void plugin_fill_status(contracts::PluginStatusOut& out) {
    out.enabled = active();
    out.error = nullptr;
}

// Feature callback does not replace the existing lifecycle or business controls.
constexpr contracts::FeatureDefinition kFeatures[] = {
    {"attribute_quality.provider", 1, contracts::FeatureCategory::kHidden,
     contracts::FeatureType::kToggle, "属性品质服务", "提供装备属性范围与品质判定能力。",
     [](contracts::PluginContext&, uint8_t) -> QolResult { return contracts::ok(); }, nullptr},
};

const contracts::PluginDescriptor kDescriptor = {
    kPluginId,
    kPluginVersion,
    1, kRequires, // requires
    0, nullptr,   // optional_requires
    1, kProvides, // provides（attribute.quality v1.0）
    1, kResources,
    0, nullptr,   // jni
    &plugin_install,
    &plugin_deactivate,
    &plugin_on_enabled,
    &plugin_on_disabled,
    &plugin_fill_status,
    &on_resource_ready,
    nullptr, 0, nullptr, 0,
    kFeatures, 1,
};

}  // namespace

const contracts::PluginDescriptor& plugin_descriptor() {
    return kDescriptor;
}

}  // namespace qol::features::attribute_quality
