#include "plugin.h"

#include <atomic>
#include <cstdint>

#include "../../contracts/result.h"

// 堆叠上限：999 功能组的声明式所有者。资源由宿主按 feature 生命周期整组安装/还原；
// S2 编解码与 99/999 数量策略已迁入 Host 常驻 adapter（game/adapter_ports），本插件不再
// 持有 provider/probe，也不跨 owner 读取门禁。单个 default-false feature 决定运行。

namespace qol::features::stack_limit {
namespace {

constexpr const char* kPluginId = "stack_limit";
constexpr const char* kPluginVersion = "0.1.0";
constexpr const char* kReason =
    "999 数量组：S2 布局与数量上限必须整组安装，失败时回落原始 99 视图";

#define STACK_LIMIT_CLAIM(NAME)                                                        \
    {NAME, NAME, contracts::ResourceMechanism::kCallPatch,                             \
     contracts::ResourcePolicy::kPrivate, contracts::ResourceRequirement::kRequired,   \
     contracts::ResourceFailure::kFailClosed, true, kPluginId, kReason, nullptr, 120,  \
     nullptr, true}

const contracts::ResourceClaim kResources[] = {
    // 既有 6 条数量 clamp。
    STACK_LIMIT_CLAIM("P_INVEN_MOVE_ITEM_CLAMP_150"),
    STACK_LIMIT_CLAIM("P_INVEN_MOVE_ITEM_CLAMP_158"),
    STACK_LIMIT_CLAIM("P_INVEN_SAVE_ITEM_DIRECT_CLAMP_DC"),
    STACK_LIMIT_CLAIM("P_INVEN_SAVE_ITEM_DIRECT_CLAMP_F0"),
    STACK_LIMIT_CLAIM("P_INVEN_SAVE_ITEM_DATA_CLAMP_7C"),
    STACK_LIMIT_CLAIM("P_INVEN_FIND_SAVE_SLOT_CLAMP_238"),
    // 数量 clamp ×6 + 直接位读重定向 ×10 = 16 条；bitend ×2 已迁至宿主常驻。
    STACK_LIMIT_CLAIM("P_UISTORE_BUTTON_SELL_EXE_D18D0_ARG"),
    STACK_LIMIT_CLAIM("P_UISTORE_BUTTON_SELL_EXE_D18D0_CALL"),
    STACK_LIMIT_CLAIM("P_UISTORE_BUTTON_SELL_EXE_D18E8_ARG"),
    STACK_LIMIT_CLAIM("P_UISTORE_BUTTON_SELL_EXE_D18E8_CALL"),
    STACK_LIMIT_CLAIM("P_UISTORE_SELL_ITEM_D26B8_ARG"),
    STACK_LIMIT_CLAIM("P_UISTORE_SELL_ITEM_D26B8_CALL"),
    STACK_LIMIT_CLAIM("P_ITEMSYSTEM_DIVIDE_108458_ARG"),
    STACK_LIMIT_CLAIM("P_ITEMSYSTEM_DIVIDE_108458_CALL"),
    STACK_LIMIT_CLAIM("P_ITEMSYSTEM_DIVIDE_10848C_ARG"),
    STACK_LIMIT_CLAIM("P_ITEMSYSTEM_DIVIDE_10848C_CALL"),
};
#undef STACK_LIMIT_CLAIM

// 仅用于诊断上报；资源安装/还原与有效态由宿主生命周期负责。
std::atomic<bool> g_enabled{false};

int plugin_install(contracts::PluginContext& ctx) {
    if (ctx.struct_size < sizeof(contracts::PluginContext) || ctx.sdk == nullptr ||
        ctx.game == nullptr) {
        return 1;
    }
    return 0;
}

void plugin_deactivate(contracts::PluginContext& ctx) {
    (void)ctx;
    g_enabled.store(false, std::memory_order_release);
}

void plugin_on_enabled(contracts::PluginContext& ctx) {
    (void)ctx;
    g_enabled.store(true, std::memory_order_release);
}

void plugin_on_disabled(contracts::PluginContext& ctx) {
    (void)ctx;
    g_enabled.store(false, std::memory_order_release);
}

void plugin_fill_status(contracts::PluginStatusOut& out) {
    out.enabled = g_enabled.load(std::memory_order_acquire);
    out.error = nullptr;
}

// 单个 default-false feature：toggle callback 为 no-op，运行条件与资源组安装由宿主
// 生命周期处理。
constexpr contracts::FeatureDefinition kFeatures[] = {
    {"stack_limit.limit_999", 0, contracts::FeatureCategory::kGameplay,
     contracts::FeatureType::kToggle, "堆叠上限", "将可堆叠物品上限提高到 999。",
     [](contracts::PluginContext&, uint8_t) -> QolResult { return contracts::ok(); }, nullptr},
};

const contracts::PluginDescriptor kDescriptor = {
    kPluginId,
    kPluginVersion,
    0, nullptr,   // requires
    0, nullptr,   // optional_requires
    0, nullptr, // provides
    static_cast<uint32_t>(sizeof(kResources) / sizeof(kResources[0])), kResources,
    0, nullptr,   // jni
    &plugin_install,
    &plugin_deactivate,
    &plugin_on_enabled,
    &plugin_on_disabled,
    &plugin_fill_status,
    nullptr,      // on_resource_ready（非入口机制，无 backup 交还）
    nullptr, 0, nullptr, 0,
    kFeatures, 1,
};

}  // namespace

const contracts::PluginDescriptor& plugin_descriptor() {
    return kDescriptor;
}

}  // namespace qol::features::stack_limit
