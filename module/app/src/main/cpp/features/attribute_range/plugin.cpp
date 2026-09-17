#include "features/attribute_range/plugin.h"

#include <cstdint>

#include "contracts/attribute_range.h"
#include "features/attribute_range/feature.h"

namespace qol::features::attribute_range {
namespace {

constexpr const char* kPluginId = "attribute_range";
constexpr const char* kPluginVersion = "0.1.0";

// 能力接口实例：函数地址为编译期常量，无跨 TU 静态初始化顺序问题；宿主进入 enabled 时注册。
constexpr contracts::AttrRangeApi kAttrRangeApi{&api_probe_jewel_range};

// 三条资源均为入口 hook、私有、必需、fail-closed、可物理还原；gate=nullptr（常驻，与旧行为一致）。
const contracts::ResourceClaim kResources[] = {
    {
        kResourceIdMathGetRandom,   // id
        "F_MATH_GET_RANDOM_VMA",    // symbol
        contracts::ResourceMechanism::kEntryHook,
        contracts::ResourcePolicy::kPrivate,
        contracts::ResourceRequirement::kRequired,
        contracts::ResourceFailure::kFailClosed,
        true,                       // resumable
        kPluginId,                  // owner
        "无可挂函数指针槽：详情探测需在同步窗口内截获游戏随机区间并透传原函数",  // reason
        nullptr,                    // gate（常驻）
        120,                        // step_budget
        reinterpret_cast<void*>(&math_get_random_wrapper),  // handler
    },
    {
        kResourceIdAddOption,
        "F_UIDESC_ADD_OPTION_VMA",
        contracts::ResourceMechanism::kEntryHook,
        contracts::ResourcePolicy::kPrivate,
        contracts::ResourceRequirement::kRequired,
        contracts::ResourceFailure::kFailClosed,
        true,
        kPluginId,
        "详情选项行渲染唯一汇合点：需在游戏写入后改写该行内联颜色码",
        nullptr,
        120,
        reinterpret_cast<void*>(&uid_desc_add_option_wrapper),
    },
    {
        kResourceIdMakeItem,
        "F_UIDESC_MAKE_ITEM_VMA",
        contracts::ResourceMechanism::kEntryHook,
        contracts::ResourcePolicy::kPrivate,
        contracts::ResourceRequirement::kRequired,
        contracts::ResourceFailure::kFailClosed,
        true,
        kPluginId,
        "详情构造入口：缓存当前物品供 AddOption 计算范围（AddOption 不带物品指针）",
        nullptr,
        120,
        reinterpret_cast<void*>(&uid_desc_make_item_wrapper),
    },
};

// 能力提供声明：id + 版本 + 接口指针（§3.3）。
const contracts::CapabilityProvisionRef kProvides[] = {
    {
        contracts::kAttrRangeCapabilityId,
        {contracts::kAttrRangeVersionMajor, contracts::kAttrRangeVersionMinor},
        &kAttrRangeApi,
    },
};

int plugin_install(contracts::PluginContext& ctx) {
    (void)ctx;
    // 自身初始化只置业务门禁；能力注册由宿主在进入 enabled 时执行（§3.4.4）。
    set_active(true);
    return 0;
}

void plugin_deactivate(contracts::PluginContext& ctx) {
    (void)ctx;
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

const contracts::PluginDescriptor kDescriptor = {
    kPluginId,
    kPluginVersion,
    0, nullptr,   // requires
    0, nullptr,   // optional_requires
    1, kProvides, // provides（attr_range v1.0）
    3, kResources,
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

}  // namespace qol::features::attribute_range
