#include "contracts/character_facts.h"
#include "contracts/game_host.h"
#include "contracts/game_ports/character_extras_port.h"
#include "contracts/game_ports/ports.h"
#include "contracts/host_sdk.h"
#include "contracts/plugin.h"
#include "plugins/push_all/feature.h"
#include "plugins/push_all/plugin.h"

#include <cassert>
#include <cstring>

namespace {

int32_t g_type = 1;
int32_t g_active_group = 0;
int32_t g_party_index = -1;
bool g_capability_available = true;

QolResult fake_type(const void*, int32_t* out) {
    *out = g_type;
    return qol::contracts::ok();
}

QolResult fake_active(const void*, int32_t* out) {
    *out = g_active_group;
    return qol::contracts::ok();
}

QolResult fake_party(const void*, int32_t* out) {
    *out = g_party_index;
    return qol::contracts::ok();
}

QolResult fake_log(int32_t, int32_t, const char*, const char*, uint32_t) {
    return qol::contracts::ok();
}

QolResult require_capability(const char* id, QolVersionRange, const void** out) {
    if (g_capability_available &&
        std::strcmp(id, qol::contracts::kCharacterFactsCapabilityId) == 0) {
        static const qol::contracts::CharacterFactsApi kFacts{&fake_type, &fake_active};
        *out = &kFacts;
        return qol::contracts::ok();
    }
    *out = nullptr;
    return qol::contracts::error(kQolErrNotFound, "missing capability", 18);
}

}  // namespace

int main() {
    qol::contracts::QolCharacterExtrasPort extras{};
    extras.abi_version = qol::contracts::kQolPortCharacterExtrasVersion;
    extras.read.party_index = &fake_party;
    extras.read.struct_size =
        static_cast<uint32_t>(sizeof(qol::contracts::QolCharacterExtrasReadApi));

    qol::contracts::QolHostSdk sdk{};
    sdk.capabilities.require = &require_capability;
    sdk.log.write = &fake_log;
    qol::contracts::QolGameHost game{};
    const void* ports[] = {&extras};
    qol::contracts::PluginContext context{};
    context.struct_size = sizeof(context);
    context.sdk_major = qol::contracts::kHostSdkMajor;
    context.sdk = &sdk;
    context.game = &game;
    context.ports = ports;
    context.port_count = 1;

    const auto& descriptor = qol::features::push_all::plugin_descriptor();
    assert(descriptor.resources_count == 2);
    assert(std::strcmp(descriptor.resources[0].id, "patch:CHAR_Move+0x168") == 0);
    assert(std::strcmp(descriptor.resources[1].id, "patch:CHAR_Move+0x180") == 0);
    assert(descriptor.resources[0].handler != nullptr);
    assert(descriptor.resources[1].handler == nullptr);

    // PSH-02：缺失 capabilities.require → 安装失败且全局状态清空
    sdk.capabilities.require = nullptr;
    assert(descriptor.install(context) == 1);
    assert(qol::features::push_all::host_sdk() == nullptr);
    assert(qol::features::push_all::game_host() == nullptr);
    assert(qol::features::push_all::character_extras() == nullptr);
    sdk.capabilities.require = &require_capability;

    // 能力不可用 → 安装失败
    g_capability_available = false;
    assert(descriptor.install(context) == 1);
    g_capability_available = true;

    assert(descriptor.install(context) == 0);
    assert(qol::features::push_all::host_sdk() == &sdk);
    assert(qol::features::push_all::character_extras() == &extras);

    int dummy = 0;
    // 空指针 → 不可推
    assert(qol::features::push_all::push_all_party_filter(nullptr) == -1);

    // 怪物 type==1 → 0
    g_type = 1;
    g_active_group = 0;
    g_party_index = -1;
    assert(qol::features::push_all::push_all_party_filter(&dummy) == 0);

    // 主控/召唤 → 0（即使 party_index == -1）
    g_type = 0;
    g_active_group = 1;
    assert(qol::features::push_all::push_all_party_filter(&dummy) == 0);

    // 队友 → 透传 party_index
    g_type = 0;
    g_active_group = 0;
    g_party_index = 2;
    assert(qol::features::push_all::push_all_party_filter(&dummy) == 2);

    // 场景设施/非队伍 → -1
    g_party_index = -1;
    assert(qol::features::push_all::push_all_party_filter(&dummy) == -1);

    // 门禁关闭 → 透传 party_index（怪物不再被特殊放开）
    descriptor.on_disabled(context);
    g_type = 1;
    assert(qol::features::push_all::push_all_party_filter(&dummy) == -1);
    g_party_index = 2;
    assert(qol::features::push_all::push_all_party_filter(&dummy) == 2);

    descriptor.deactivate(context);
    assert(qol::features::push_all::character_extras() == nullptr);
    return 0;
}
