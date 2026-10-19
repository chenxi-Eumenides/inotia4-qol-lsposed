#include "plugin.h"

#include "../../contracts/attribute_quality.h"
#include "../../contracts/item_facts.h"
#include "feature.h"

namespace qol::features::attribute_range_display {
namespace {

constexpr const char* kPluginId = "attribute_range_display";
constexpr const char* kPluginVersion = "0.1.0";

const contracts::CapabilityRequirement kRequires[] = {
    {contracts::kAttributeQualityCapabilityId,
     {contracts::kAttributeQualityVersionMajor, contracts::kAttributeQualityVersionMinor}},
    {contracts::kItemFactsCapabilityId,
     {contracts::kItemFactsVersionMajor, contracts::kItemFactsVersionMinor}},
};

const contracts::ResourceClaim kResources[] = {
    {
        kResourceIdAddOption,
        "F_UIDESC_ADD_OPTION_VMA",
        contracts::ResourceMechanism::kEntryHook,
        contracts::ResourcePolicy::kPrivate,
        contracts::ResourceRequirement::kOptional,
        contracts::ResourceFailure::kFailClosed,
        true,
        kPluginId,
        "详情选项行渲染：按 item.facts 分类确认原生词条后消费 attribute.quality 改写数值内联颜色码",
        nullptr,
        120,
        reinterpret_cast<void*>(&uid_desc_add_option_wrapper),
    },
    {
        kResourceIdMakeItem,
        "F_UIDESC_MAKE_ITEM_VMA",
        contracts::ResourceMechanism::kEntryHook,
        contracts::ResourcePolicy::kPrivate,
        contracts::ResourceRequirement::kOptional,
        contracts::ResourceFailure::kFailClosed,
        true,
        kPluginId,
        "详情构造入口：为 AddOption 提供当前物品上下文",
        nullptr,
        120,
        reinterpret_cast<void*>(&uid_desc_make_item_wrapper),
    },
};

int plugin_install(contracts::PluginContext& ctx) {
    set_context(ctx);
    set_active(true);
    return 0;
}

void plugin_deactivate(contracts::PluginContext& ctx) {
    (void)ctx;
    set_active(false);
}

void plugin_on_enabled(contracts::PluginContext& ctx) {
    set_context(ctx);
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

// Feature callback does not replace the existing lifecycle or business controls.
constexpr contracts::FeatureDefinition kFeatures[] = {
    {"attribute_range_display.ranges", 1, contracts::FeatureCategory::kBasicOptimization,
     contracts::FeatureType::kToggle, "属性范围显示", "在物品描述中显示装备属性范围。",
     [](contracts::PluginContext&, uint8_t) -> QolResult { return contracts::ok(); }, nullptr},
};

const contracts::PluginDescriptor kDescriptor = {
    kPluginId,
    kPluginVersion,
    2,
    kRequires,
    0,
    nullptr,
    0,
    nullptr,
    2,
    kResources,
    0,
    nullptr,
    &plugin_install,
    &plugin_deactivate,
    &plugin_on_enabled,
    &plugin_on_disabled,
    &plugin_fill_status,
    &on_resource_ready,
    nullptr,
    0,
    nullptr, 0,
    kFeatures, 1,
};

}  // namespace

const contracts::PluginDescriptor& plugin_descriptor() { return kDescriptor; }

}  // namespace qol::features::attribute_range_display
