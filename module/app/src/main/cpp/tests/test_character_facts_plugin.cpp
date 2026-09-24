#include "contracts/character_facts.h"
#include "contracts/game_ports/character_http_port.h"
#include "contracts/game_ports/character_port.h"
#include "contracts/game_ports/character_write_port.h"
#include "contracts/http.h"
#include "contracts/host_sdk.h"
#include "plugins/core.character/plugin.h"

#include <cstdio>
#include <cstring>
#include <string>

static int g_pass = 0;
static int g_fail = 0;
static int g_type_calls = 0;
static int g_active_calls = 0;
static qol::contracts::QolTypedResultCommandHandler g_result_handler = nullptr;
static uint32_t g_post_handler_id = 0;
static unsigned char g_post_payload[qol::contracts::kTypedCommandPayloadCapacity]{};
static uint32_t g_post_payload_size = 0;
static int g_write_calls = 0;

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

QolResult fake_write(const qol::contracts::QolCharacterWriteArgs* args, char* out,
                     uint32_t capacity, uint32_t* out_size) {
    ++g_write_calls;
    if (args == nullptr || out == nullptr || out_size == nullptr) {
        return qol::contracts::error(kQolErrInvalidArgument, "bad write", 9);
    }
    constexpr char kResult[] = "{\"ok\":true,\"state\":{\"skills\":[]}}";
    if (capacity < sizeof(kResult) - 1) {
        return qol::contracts::error(kQolErrBlocked, "small", 5);
    }
    std::memcpy(out, kResult, sizeof(kResult) - 1);
    *out_size = sizeof(kResult) - 1;
    return qol::contracts::ok();
}

QolResult fake_register_result(uint32_t, qol::contracts::QolTypedResultCommandHandler handler,
                               void*) {
    g_result_handler = handler;
    return qol::contracts::ok();
}

QolResult fake_unregister_result(uint32_t) {
    g_result_handler = nullptr;
    return qol::contracts::ok();
}

QolResult fake_post_typed(uint32_t handler_id, const void* payload, uint32_t size,
                          qol::contracts::QolCommandTicket* ticket) {
    if (payload == nullptr || size > sizeof(g_post_payload) || ticket == nullptr) {
        return qol::contracts::error(kQolErrInvalidArgument, "bad command", 11);
    }
    g_post_handler_id = handler_id;
    g_post_payload_size = size;
    std::memcpy(g_post_payload, payload, size);
    *ticket = 41;
    return qol::contracts::ok();
}

int main() {
    const auto& descriptor = qol::features::core_character::plugin_descriptor();
    CHECK(std::strcmp(descriptor.id, "core.character") == 0);
    CHECK(std::strcmp(descriptor.version, "0.1.0") == 0);
    CHECK(descriptor.requires_count == 2 && descriptor.optional_count == 0);
    CHECK(std::strcmp(descriptor.requires[0].id, qol::contracts::kHttpRegistryCapabilityId) == 0);
    CHECK(std::strcmp(descriptor.requires[1].id, qol::contracts::kHttpDispatchCapabilityId) == 0);
    CHECK(descriptor.provides_count == 1 && descriptor.provides != nullptr);
    CHECK(std::strcmp(descriptor.provides[0].id,
                      qol::contracts::kCharacterFactsCapabilityId) == 0);
    CHECK(descriptor.provides[0].version.major == 1 && descriptor.provides[0].version.minor == 0);
    CHECK(descriptor.provides[0].iface != nullptr);
    CHECK(descriptor.port_count == 3 && descriptor.ports != nullptr);
    CHECK(descriptor.ports[0].id == qol::contracts::QolPortId::QOL_PORT_CHARACTER);
    CHECK(descriptor.ports[1].id == qol::contracts::QolPortId::QOL_PORT_CHARACTER_HTTP);
    CHECK(descriptor.ports[2].id == qol::contracts::QolPortId::QOL_PORT_CHARACTER_WRITE);

    const auto* api = static_cast<const qol::contracts::CharacterFactsApi*>(
        descriptor.provides[0].iface);
    CHECK(api->type != nullptr);
    CHECK(api->is_active_player_group != nullptr);

    qol::contracts::QolHostSdk sdk{};
    sdk.abi = {sizeof(qol::contracts::QolHostSdk), qol::contracts::kHostSdkMajor,
               qol::contracts::kHostSdkMinor};
    sdk.commands.post_typed = &fake_post_typed;
    sdk.ticket_result.register_typed_result_handler = &fake_register_result;
    sdk.ticket_result.unregister_typed_result_handler = &fake_unregister_result;
    const qol::contracts::QolCharacterPort port{{1, 0}, {&fake_type, &fake_active}};
    const qol::contracts::QolCharacterHttpPort http_port{{1, 0}, {&fake_body_json}};
    const qol::contracts::QolCharacterWritePort write_port{{1, 0}, {&fake_write}};
    const void* ports[] = {&port, &http_port, &write_port};
    qol::contracts::PluginContext ctx{
        qol::contracts::PLUGIN_CONTEXT_STRUCT_SIZE,
        qol::contracts::kHostSdkMajor,
        qol::contracts::kHostSdkMinor,
        &sdk,
        nullptr,
        "core.character",
        ports,
        3,
    };

    CHECK(descriptor.install(ctx) == 0);
    CHECK(g_result_handler != nullptr);
    int32_t type_value = 0;
    int32_t active_value = 0;
    CHECK(api->type(reinterpret_cast<const void*>(1), &type_value).ok == kQolOk);
    CHECK(type_value == 7 && g_type_calls == 1);
    CHECK(api->is_active_player_group(reinterpret_cast<const void*>(1), &active_value).ok == kQolOk);
    CHECK(active_value == 1 && g_active_calls == 1);

    const char body[] = "{\"action_id\":7,\"level\":2}";
    constexpr char kAddSkillPath[] = "/api/character/grow/add_skill";
    qol::contracts::HttpReq request{qol::contracts::HttpMethod::kPost,
                                    kAddSkillPath, sizeof(kAddSkillPath) - 1,
                                    reinterpret_cast<const uint8_t*>(body),
                                    static_cast<uint32_t>(sizeof(body) - 1),
                                    nullptr, 0, nullptr, 0};
    char response_storage[256]{};
    qol::contracts::HttpRes response{reinterpret_cast<uint8_t*>(response_storage),
                                     sizeof(response_storage), 0, 0};
    const qol::contracts::HttpQueryHandlerDescriptor* route = nullptr;
    for (uint32_t i = 15; i < descriptor.http_query_count; ++i) {
        if (std::strcmp(descriptor.http_queries[i].path, kAddSkillPath) == 0) {
            route = &descriptor.http_queries[i];
            break;
        }
    }
    CHECK(route != nullptr);
    CHECK(route != nullptr && route->handler(&request, &response, route->ctx).ok == kQolOk);
    CHECK(std::string(response_storage, response.body_size) == "{\"ok\":true,\"ticket\":41}");
    CHECK(g_post_handler_id != 0 && g_post_payload_size > 0);
    int32_t posted_fields[4]{};
    std::memcpy(posted_fields, g_post_payload, sizeof(posted_fields));
    CHECK(posted_fields[0] == static_cast<int32_t>(
              qol::contracts::QolCharacterWriteKind::kLearnAction));
    CHECK(posted_fields[1] == 0 && posted_fields[2] == 7 && posted_fields[3] == 2);
    char result_storage[256]{};
    uint32_t result_size = 0;
    CHECK(g_result_handler(nullptr, g_post_payload, g_post_payload_size, result_storage,
                           sizeof(result_storage), &result_size));
    CHECK(std::string(result_storage, result_size) ==
          "{\"ok\":true,\"state\":{\"skills\":[]}}");
    CHECK(g_write_calls == 1);

    descriptor.deactivate(ctx);
    CHECK(g_result_handler == nullptr);
    CHECK(api->type(reinterpret_cast<const void*>(1), &type_value).code == kQolErrNotReady);
    CHECK(api->is_active_player_group(reinterpret_cast<const void*>(1), &active_value).code ==
          kQolErrNotReady);
    CHECK(g_fail == 0);
    std::printf("character_facts_plugin: pass=%d fail=%d\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
