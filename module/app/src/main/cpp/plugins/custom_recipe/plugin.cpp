#include "plugin.h"

#include <cstring>
#include <cstdint>

#include "../../contracts/attribute_quality.h"
#include "../../contracts/craft_commands.h"
#include "../../contracts/craft_facts.h"
#include "../../contracts/craft_result.h"
#include "../../contracts/game_ports/custom_recipe_write_port.h"
#include "../../contracts/events.h"
#include "../../contracts/item_facts.h"
#include "../../contracts/text_layer.h"
#include "feature.h"
#include "table.h"
#include "text_table.h"

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

// ---- 「宝石强化」呈现文案（ADR 0001 D3：合成域的配方呈现文案归本插件）----
//
// 表与查表纯逻辑在 text_table.h（host 可测）；此处只保留分派消费者回调与能力句柄。
constexpr int32_t kTextPhaseAfter = 1;
constexpr int32_t kTextKindModify = 1;

const contracts::TextLayerApi* g_text_api = nullptr;
uint64_t g_text_consumer = 0;

// after 阶段消费者：仅当当前线程处于对应作用域且 id 命中时返回模块文案。
const char* recipe_text_modify(uint32_t text_id, const char* current, void* ctx) {
    (void)current;
    (void)ctx;
    return recipe_owner_lookup(text_id);
}

void on_config_changed(void* /*ctx*/, const void* payload) {
    if (payload == nullptr) return;
    const auto* event = static_cast<const contracts::ConfigChangedEvent*>(payload);
    if (event->key == nullptr || std::strcmp(event->key, kGateKey) != 0) return;
    set_enabled(event->int_value != 0);
}

bool write_stuff_need_handler(void*, const uint8_t* payload, uint32_t size);
bool set_stuff_item_handler(void*, const uint8_t* payload, uint32_t size);

int plugin_install(contracts::PluginContext& ctx) {
    if (ctx.sdk == nullptr || ctx.game == nullptr || ctx.sdk->capabilities.require == nullptr) {
        return 1;
    }
    // 文本层能力（配方呈现文案）：先取能力并订阅；失败即 install 失败，此时尚无副作用。
    const void* text_iface = nullptr;
    if (!contracts::succeeded(ctx.sdk->capabilities.require(
            contracts::kTextLayerCapabilityId,
            {contracts::kTextLayerVersionMajor, contracts::kTextLayerVersionMinor}, &text_iface)) ||
        text_iface == nullptr) {
        return 1;
    }
    const auto* text_api = static_cast<const contracts::TextLayerApi*>(text_iface);
    if (text_api->subscribe == nullptr) return 1;
    g_text_consumer = text_api->subscribe(kTextPhaseAfter, kTextKindModify, &recipe_text_modify,
                                          nullptr);
    if (g_text_consumer == 0) return 1;
    g_text_api = text_api;
    const void* iface = nullptr;
    if (!contracts::succeeded(ctx.sdk->capabilities.require(
            contracts::kItemFactsCapabilityId,
            {contracts::kItemFactsVersionMajor, contracts::kItemFactsVersionMinor}, &iface)) ||
        iface == nullptr) {
        return 1;
    }
    set_item_facts(static_cast<const contracts::ItemFactsApi*>(iface));
    iface = nullptr;
    if (!contracts::succeeded(ctx.sdk->capabilities.require(
            contracts::kCraftFactsCapabilityId,
            {contracts::kCraftFactsVersion.major, contracts::kCraftFactsVersion.minor}, &iface)) ||
        iface == nullptr) {
        set_item_facts(nullptr);
        return 1;
    }
    set_craft_facts(static_cast<const contracts::CraftFactsApi*>(iface));
    if (ctx.ports == nullptr || ctx.port_count != 1 || ctx.ports[0] == nullptr) {
        set_item_facts(nullptr);
        set_craft_facts(nullptr);
        return 1;
    }
    const auto* write_port = static_cast<const contracts::QolCustomRecipeWritePort*>(ctx.ports[0]);
    if (!contracts::satisfies(write_port->abi_version,
                              {contracts::kQolPortCustomRecipeWriteVersion.major,
                               contracts::kQolPortCustomRecipeWriteVersion.minor}) ||
        write_port->write_stuff_need == nullptr || write_port->set_stuff_item == nullptr ||
        write_port->description_set_off == nullptr ||
        ctx.sdk->commands.register_typed_handler == nullptr ||
        ctx.sdk->commands.unregister_typed_handler == nullptr ||
        ctx.sdk->commands.register_typed_handler(contracts::kCraftWriteStuffNeedHandlerId,
                                                  &write_stuff_need_handler, nullptr).ok != kQolOk) {
        set_item_facts(nullptr);
        set_craft_facts(nullptr);
        return 1;
    }
    if (ctx.sdk->commands.register_typed_handler(contracts::kCraftSetStuffItemHandlerId,
                                                  &set_stuff_item_handler, nullptr).ok != kQolOk) {
        (void)ctx.sdk->commands.unregister_typed_handler(contracts::kCraftWriteStuffNeedHandlerId);
        set_item_facts(nullptr);
        set_craft_facts(nullptr);
        return 1;
    }
    set_write_port(write_port);
    set_context(ctx);
    // 进档重建动态特殊装备配方；订阅经 Host SDK 事件总线完成。
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
    if (g_text_api != nullptr && g_text_api->unsubscribe != nullptr && g_text_consumer != 0) {
        g_text_api->unsubscribe(g_text_consumer);
    }
    g_text_consumer = 0;
    g_text_api = nullptr;
    if (g_config_subscription != 0) {
        if (host_sdk()->events.unsubscribe != nullptr) {
            (void)host_sdk()->events.unsubscribe(g_config_subscription);
        }
        g_config_subscription = 0;
    }
    if (host_sdk() != nullptr && host_sdk()->commands.unregister_typed_handler != nullptr) {
        (void)host_sdk()->commands.unregister_typed_handler(contracts::kCraftWriteStuffNeedHandlerId);
        (void)host_sdk()->commands.unregister_typed_handler(contracts::kCraftSetStuffItemHandlerId);
    }
    unregister_dynamic_recipes();
    set_item_facts(nullptr);
    set_craft_facts(nullptr);
    set_write_port(nullptr);
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

const contracts::CapabilityRequirement kRequires[] = {
    {contracts::kItemFactsCapabilityId,
     QolVersionRange{static_cast<uint16_t>(contracts::kItemFactsVersionMajor),
                      static_cast<uint16_t>(contracts::kItemFactsVersionMinor)}},
    {contracts::kCraftFactsCapabilityId,
     {contracts::kCraftFactsVersion.major, contracts::kCraftFactsVersion.minor}},
    // 配方呈现文案经文本层能力登记（ADR 0001 D3）。
    {contracts::kTextLayerCapabilityId,
     {static_cast<uint16_t>(contracts::kTextLayerVersionMajor),
      static_cast<uint16_t>(contracts::kTextLayerVersionMinor)}},
};

const contracts::QolPortRequirement kPorts[] = {
    {contracts::QolPortId::QOL_PORT_CUSTOM_RECIPE_WRITE,
     {contracts::kQolPortCustomRecipeWriteVersion.major,
      contracts::kQolPortCustomRecipeWriteVersion.minor}},
};

bool write_stuff_need_handler(void*, const uint8_t* payload, uint32_t size) {
    return handle_write_stuff_need_command(payload, size);
}

bool set_stuff_item_handler(void*, const uint8_t* payload, uint32_t size) {
    return handle_set_stuff_item_command(payload, size);
}

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
        3, kRequires,            // requires=item.facts + craft.facts + text_layer
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
        kPorts, 1,
    };
    return descriptor;
}

}  // namespace qol::features::custom_recipe
