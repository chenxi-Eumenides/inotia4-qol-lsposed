#include "plugin.h"

#include <cstdint>

#include "../../contracts/character_facts.h"
#include "../../contracts/game_ports/character_extras_port.h"
#include "feature.h"

namespace qol::features::push_all {
namespace {

constexpr const char* kPluginId = "push_all";
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

// 两条资源均为调用点 patch、私有、必需、fail-closed、可物理还原。
// 资源声明（ARCHITECTURE.md §4.3 字段顺序）。gate=nullptr：资源跟随插件 lifecycle，
// 插件仅在自有 feature 开启（want_run）时安装，关闭即整体还原。
const contracts::ResourceClaim kResources[] = {
    {
        kResourceIdParty,          // id
        "P_CHAR_MOVE_PUSH_PARTY",  // symbol：patch 点登记名
        contracts::ResourceMechanism::kCallPatch,
        contracts::ResourcePolicy::kPrivate,
        contracts::ResourceRequirement::kRequired,
        contracts::ResourceFailure::kFailClosed,
        true,                      // resumable
        kPluginId,                 // owner
        "队伍闸门是内联 BL 调用点，无函数指针槽可挂，只能重定向到 wrapper",  // reason
        nullptr,                   // gate（生命周期持有）
        120,                       // step_budget
        reinterpret_cast<void*>(&push_all_party_filter),  // handler（BL 目标）
    },
    {
        kResourceIdStand,
        "P_CHAR_MOVE_PUSH_STAND",
        contracts::ResourceMechanism::kCallPatch,
        contracts::ResourcePolicy::kPrivate,
        contracts::ResourceRequirement::kRequired,
        contracts::ResourceFailure::kFailClosed,
        true,
        kPluginId,
        "站立检查是条件分支，无可透传执行体，只能将 cbz 置 NOP；禁用即物理还原",
        nullptr,
        120,
        nullptr,  // kWordReplace 无 handler
    },
};

// JNI 绑定元数据：符号名与签名在重构期冻结；签名哈希 P5 落地前为 nullptr。
const contracts::JniBinding kJni[] = {
    {
        "nativeSetPushAllEnabled",
        "Java_com_inotia4_qol_NativeBridge_nativeSetPushAllEnabled",
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
    if (g_sdk->capabilities.require == nullptr) {
        g_sdk = nullptr;
        g_game = nullptr;
        g_character_extras = nullptr;
        return 1;
    }
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
    {"push_all.push", 0, contracts::FeatureCategory::kGameplay,
     contracts::FeatureType::kToggle, "推怪", "移动时可推动怪物、队友与召唤物。",
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

}  // namespace qol::features::push_all
