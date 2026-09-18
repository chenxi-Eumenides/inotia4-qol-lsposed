#include "plugins/custom_recipe/plugin.h"

#include <cstring>
#include <cstdint>

#include "contracts/attribute_quality.h"
#include "contracts/craft_result.h"
#include "contracts/events.h"
#include "plugins/custom_recipe/feature.h"
#include "plugins/custom_recipe/table.h"

// 自定义合成配方插件：结果层能力提供者 + 表注入行为所有权（C2 Step 4）。
//
// 门禁：配置键 `customRecipeEnabled`（Kotlin JNI 写配置、宿主 gate 同步）。
//   · 门禁开 → kOwnership acquire 注入 RECIPEBASE/MIXTUREBASE；craft_ui 的界面 hook 生效。
//   · 门禁关 → release 收回注入记录数；provider enabled=false，残留面板 fail-closed。

namespace qol::features::custom_recipe {
namespace {

constexpr const char* kPluginId = "custom_recipe";
constexpr const char* kPluginVersion = "0.1.0";
constexpr const char* kGateKey = "customRecipeEnabled";
uint64_t g_config_subscription = 0;

void on_config_changed(void* /*ctx*/, const void* payload) {
    if (payload == nullptr) return;
    const auto* event = static_cast<const contracts::ConfigChangedEvent*>(payload);
    if (event->key == nullptr || std::strcmp(event->key, kGateKey) != 0) return;
    set_enabled(event->int_value != 0);
}

int plugin_install(contracts::PluginContext& ctx) {
    if (ctx.sdk == nullptr || ctx.game == nullptr) return 1;
    set_context(ctx);
    // 进档重建动态特殊装备配方（save_enter 设施迁移前直接注册 legacy 回调；register 幂等）。
    register_dynamic_recipes();
    // 门禁初值取配置；订阅配置变更（重复订阅会留孤儿订阅，已订阅则跳过）。
    int32_t enabled_value = 0;
    if (host_sdk()->config.get_bool != nullptr) {
        (void)host_sdk()->config.get_bool(kGateKey, 0, &enabled_value);
    }
    set_enabled(enabled_value != 0);
    if (g_config_subscription == 0) {
        if (host_sdk()->events.subscribe != nullptr) {
            (void)host_sdk()->events.subscribe(contracts::kEventConfigChanged, &on_config_changed,
                                                nullptr, &g_config_subscription);
        }
    }
    return 0;
}

void plugin_deactivate(contracts::PluginContext& ctx) {
    (void)ctx;
    if (g_config_subscription != 0) {
        if (host_sdk()->events.unsubscribe != nullptr) {
            (void)host_sdk()->events.unsubscribe(g_config_subscription);
        }
        g_config_subscription = 0;
    }
    set_enabled(false);
    ::custom_recipe::custom_recipe_table_deactivate();
}

void plugin_on_enabled(contracts::PluginContext& ctx) {
    if (ctx.sdk == nullptr || ctx.game == nullptr) return;
    set_context(ctx);
    int32_t enabled_value = 0;
    if (host_sdk()->config.get_bool != nullptr) {
        (void)host_sdk()->config.get_bool(kGateKey, 0, &enabled_value);
    }
    set_enabled(enabled_value != 0);
}

void plugin_on_disabled(contracts::PluginContext& ctx) {
    (void)ctx;
    set_enabled(false);
    // 先于 capability 注销撤回表注入，避免撤销窗口出现「表仍在、craft_result 已缺失」。
    ::custom_recipe::custom_recipe_table_deactivate();
}

void plugin_fill_status(contracts::PluginStatusOut& out) {
    out.enabled = enabled();
    out.error = nullptr;
}

// optional_requires=[attribute.quality]：可选依赖缺失/失效只置 degraded，不级联停用。
const contracts::CapabilityRequirement kOptionalRequires[] = {
    {contracts::kAttributeQualityCapabilityId,
     QolVersionRange{static_cast<uint16_t>(contracts::kAttributeQualityVersionMajor),
                     static_cast<uint16_t>(contracts::kAttributeQualityVersionMinor)}},
};

// provides=craft_result v1.0（结果层能力，消费者=craft_ui）。
const contracts::CapabilityProvisionRef kProvides[] = {
    {contracts::kCraftResultCapabilityId,
     QolVersion{static_cast<uint16_t>(contracts::kCraftResultVersionMajor),
                static_cast<uint16_t>(contracts::kCraftResultVersionMinor)},
     &craft_result_api()},
};

// 行为所有权：RECIPEBASE/MIXTUREBASE 表注入（非 hook/patch 的互斥槽占有）。
// acquire=table_ensure（失败→非 0，fail-soft → degraded）、release=table_deactivate（total）。
const contracts::ResourceClaim kClaims[] = {
    {"own:RECIPEBASE_MIXTUREBASE",
     "RECIPEBASE_MIXTUREBASE",
     contracts::ResourceMechanism::kOwnership,
     contracts::ResourcePolicy::kPrivate,
     contracts::ResourceRequirement::kRequired,
     contracts::ResourceFailure::kFailSoft,
     true,
     kPluginId,
     "RECIPEBASE/MIXTUREBASE 注入记录的占有：门禁关时收回记录数、残留面板不命中注入记录",
     kGateKey,
     120,
     const_cast<contracts::OwnershipOps*>(&ownership_ops())},
};

}  // namespace

const contracts::PluginDescriptor& plugin_descriptor() {
    static const contracts::PluginDescriptor descriptor = {
        kPluginId,
        kPluginVersion,
        0, nullptr,             // requires
        1, kOptionalRequires,    // optional_requires=[attribute.quality]
        1, kProvides,            // provides=craft_result
        1, kClaims,              // resources=ownership
        0, nullptr,              // jni
        &plugin_install,
        &plugin_deactivate,
        &plugin_on_enabled,
        &plugin_on_disabled,
        &plugin_fill_status,
        nullptr,                 // on_resource_ready（ownership 无 backup）
    };
    return descriptor;
}

}  // namespace qol::features::custom_recipe
