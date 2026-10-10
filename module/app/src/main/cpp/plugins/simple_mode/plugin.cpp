#include "plugin.h"

#include <cstdint>

#include "../../contracts/character_facts.h"
#include "../../contracts/game_ports/character_extras_port.h"
#include "feature.h"

namespace qol::features::simple_mode {
namespace {

constexpr const char* kPluginId = "simple_mode";
constexpr const char* kPluginVersion = "0.1.0";

const contracts::QolHostSdk* g_sdk = nullptr;
const contracts::QolGameHost* g_game = nullptr;
const contracts::QolCharacterExtrasPort* g_character_extras = nullptr;

const contracts::CapabilityRequirement kRequires[] = {
    {contracts::kCharacterFactsCapabilityId,
     {contracts::kCharacterFactsVersionMajor, contracts::kCharacterFactsVersionMinor}},
};

const contracts::QolPortRequirement kPorts[] = {
    {contracts::QolPortId::QOL_PORT_CHARACTER_EXTRAS,
     {contracts::kQolPortCharacterExtrasVersion.major,
      contracts::kQolPortCharacterExtrasVersion.minor}},
};

// 资源声明（ARCHITECTURE.md §4.3 字段顺序）：两个入口 hook 均为私有、必需、fail-closed。
// gate=nullptr：资源跟随插件 lifecycle，插件仅在自有 feature 开启时安装。
const contracts::ResourceClaim kResources[] = {
    {
        kResourceIdAddDamage,       // id
        "F_CHAR_ADD_DAMAGE_VMA",    // symbol
        contracts::ResourceMechanism::kEntryHook,
        contracts::ResourcePolicy::kPrivate,
        contracts::ResourceRequirement::kRequired,
        contracts::ResourceFailure::kFailClosed,
        true,                       // resumable
        kPluginId,                  // owner
        "无函数指针槽，15 个静态调用点经此单点汇合",  // reason
        nullptr,                    // gate（生命周期持有）
        120,                        // step_budget
        reinterpret_cast<void*>(&char_add_damage_wrapper),  // handler
    },
    {
        kResourceIdUpdateAttrFromMonster,
        "F_CHAR_UPDATE_ATTR_FROM_MONSTER_VMA",
        contracts::ResourceMechanism::kEntryHook,
        contracts::ResourcePolicy::kPrivate,
        contracts::ResourceRequirement::kRequired,
        contracts::ResourceFailure::kFailClosed,
        true,
        kPluginId,
        "最大生命重算唯一入口",
        nullptr,
        120,
        reinterpret_cast<void*>(&char_update_attr_from_monster_wrapper),
    },
};

// JNI 绑定元数据：符号名与签名在重构期冻结；签名哈希 P5 落地前为 nullptr。
const contracts::JniBinding kJni[] = {
    {
        "nativeSetSimpleModeEnabled",
        "Java_com_inotia4_qol_NativeBridge_nativeSetSimpleModeEnabled",
        nullptr,
    },
};

int plugin_install(contracts::PluginContext& ctx) {
    g_sdk = ctx.sdk;
    g_game = ctx.game;
    g_character_extras = nullptr;
    if (ctx.struct_size < sizeof(contracts::PluginContext) ||
        ctx.sdk_major != contracts::kHostSdkMajor || g_sdk == nullptr || g_game == nullptr ||
        ctx.ports == nullptr || ctx.port_count != 1 || ctx.ports[0] == nullptr) {
        g_sdk = nullptr;
        g_game = nullptr;
        g_character_extras = nullptr;
        return 1;
    }
    g_character_extras = static_cast<const contracts::QolCharacterExtrasPort*>(ctx.ports[0]);
    if (g_sdk->capabilities.require == nullptr) return 1;
    const void* iface = nullptr;
    if (!contracts::succeeded(g_sdk->capabilities.require(
            contracts::kCharacterFactsCapabilityId,
            {contracts::kCharacterFactsVersionMajor, contracts::kCharacterFactsVersionMinor},
            &iface)) ||
        iface == nullptr) {
        g_sdk = nullptr;
        g_game = nullptr;
        g_character_extras = nullptr;
        return 1;
    }
    set_character_facts(static_cast<const contracts::CharacterFactsApi*>(iface));
    // feature 为插件唯一开关：want_run 成立才会 install，故进入此路径即视为启用。
    set_gate(true);
    return 0;
}

void plugin_deactivate(contracts::PluginContext& ctx) {
    (void)ctx;
    set_gate(false);
    set_character_facts(nullptr);
    g_character_extras = nullptr;
}

// 启停仅切门禁，不做资源操作（§3.3）。
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

// Feature callback does not replace the existing lifecycle or business controls.
constexpr contracts::FeatureDefinition kFeatures[] = {
    {"simple_mode.combat", 0, contracts::FeatureCategory::kGameplay,
     contracts::FeatureType::kToggle, "简单模式", "降低怪物生命并增强玩家伤害与承伤。",
     [](contracts::PluginContext&, uint8_t) -> QolResult { return contracts::ok(); }, nullptr},
};

const contracts::PluginDescriptor kDescriptor = {
    kPluginId,
    kPluginVersion,
    1, kRequires, // requires=character.facts
    0, nullptr,   // optional_requires
    0, nullptr,   // provides
    2, kResources,
    1, kJni,
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

const contracts::QolGameHost* game_host() { return g_game; }

const contracts::QolCharacterExtrasPort* character_extras() { return g_character_extras; }

const contracts::PluginDescriptor& plugin_descriptor() {
    return kDescriptor;
}

}  // namespace qol::features::simple_mode
