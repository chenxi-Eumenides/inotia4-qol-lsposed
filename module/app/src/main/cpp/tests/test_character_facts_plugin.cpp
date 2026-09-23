#include "contracts/character_facts.h"
#include "contracts/game_ports/character_http_port.h"
#include "contracts/game_ports/character_port.h"
#include "plugins/character_facts/plugin.h"

#include <cstdio>
#include <cstring>

static int g_pass = 0;
static int g_fail = 0;
static int g_type_calls = 0;
static int g_active_calls = 0;

#define CHECK(cond) do { \
    if (cond) ++g_pass; \
    else { ++g_fail; std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } \
} while (0)

QolResult fake_type(const void*, int32_t* out_type) {
    ++g_type_calls;
    if (out_type == nullptr) return QolResult{kQolErrInvalidArgument, kQolErrInvalidArgument, nullptr, 0};
    *out_type = 7;
    return qol::contracts::ok();
}

QolResult fake_active(const void*, int32_t* out_active) {
    ++g_active_calls;
    if (out_active == nullptr) {
        return QolResult{kQolErrInvalidArgument, kQolErrInvalidArgument, nullptr, 0};
    }
    *out_active = 1;
    return qol::contracts::ok();
}

// install 只校验函数指针非空，不在此测试中调用。
QolResult fake_body_json(int32_t, char*, uint32_t, uint32_t*) {
    return qol::contracts::ok();
}

int main() {
    const auto& descriptor = qol::features::character_facts::plugin_descriptor();
    CHECK(std::strcmp(descriptor.id, "character_facts") == 0);
    CHECK(std::strcmp(descriptor.version, "0.1.0") == 0);
    CHECK(descriptor.requires_count == 2 && descriptor.optional_count == 0);
    CHECK(std::strcmp(descriptor.requires[0].id, qol::contracts::kHttpRegistryCapabilityId) == 0);
    CHECK(std::strcmp(descriptor.requires[1].id, qol::contracts::kHttpDispatchCapabilityId) == 0);
    CHECK(descriptor.provides_count == 1 && descriptor.provides != nullptr);
    CHECK(std::strcmp(descriptor.provides[0].id,
                      qol::contracts::kCharacterFactsCapabilityId) == 0);
    CHECK(descriptor.provides[0].version.major == 1 && descriptor.provides[0].version.minor == 0);
    CHECK(descriptor.provides[0].iface != nullptr);
    CHECK(descriptor.port_count == 2 && descriptor.ports != nullptr);
    CHECK(descriptor.ports[0].id == qol::contracts::QolPortId::QOL_PORT_CHARACTER);
    CHECK(descriptor.ports[1].id == qol::contracts::QolPortId::QOL_PORT_CHARACTER_HTTP);

    const auto* api = static_cast<const qol::contracts::CharacterFactsApi*>(
        descriptor.provides[0].iface);
    CHECK(api->type != nullptr);
    CHECK(api->is_active_player_group != nullptr);

    qol::contracts::QolHostSdk sdk{};
    const qol::contracts::QolCharacterPort port{{1, 0}, {&fake_type, &fake_active}};
    const qol::contracts::QolCharacterHttpPort http_port{{1, 0}, {&fake_body_json}};
    const void* ports[] = {&port, &http_port};
    qol::contracts::PluginContext ctx{
        qol::contracts::PLUGIN_CONTEXT_STRUCT_SIZE,
        qol::contracts::kHostSdkMajor,
        qol::contracts::kHostSdkMinor,
        &sdk,
        nullptr,
        "character_facts",
        ports,
        2,
    };

    CHECK(descriptor.install(ctx) == 0);
    int32_t type_value = 0;
    int32_t active_value = 0;
    CHECK(api->type(reinterpret_cast<const void*>(1), &type_value).ok == kQolOk);
    CHECK(type_value == 7 && g_type_calls == 1);
    CHECK(api->is_active_player_group(reinterpret_cast<const void*>(1), &active_value).ok == kQolOk);
    CHECK(active_value == 1 && g_active_calls == 1);

    descriptor.deactivate(ctx);
    CHECK(api->type(reinterpret_cast<const void*>(1), &type_value).code == kQolErrNotReady);
    CHECK(api->is_active_player_group(reinterpret_cast<const void*>(1), &active_value).code ==
          kQolErrNotReady);
    CHECK(g_fail == 0);
    std::printf("character_facts_plugin: pass=%d fail=%d\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
