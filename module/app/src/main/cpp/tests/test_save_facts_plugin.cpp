#include "contracts/save_facts.h"
#include "contracts/game_ports/ports.h"
#include "contracts/game_ports/save_port.h"
#include "contracts/game_ports/save_write_port.h"
#include "contracts/host_sdk.h"
#include "contracts/http.h"
#include "contracts/result.h"
#include "contracts/slot_data.h"
#include "plugins/core.save/plugin.h"

#include <cstdio>
#include <cstring>
#include <string>

static int g_pass = 0;
static int g_fail = 0;

#define CHECK(condition) do { \
    if (condition) ++g_pass; \
    else { ++g_fail; std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition); } \
} while (0)

namespace {

using namespace qol::contracts;

struct TestSaveWritePayload { int32_t kind; int32_t slot; int32_t class_idx; };

QolTypedResultCommandHandler g_result_handler = nullptr;
uint32_t g_attempt = 0;
int g_deferred = 0;
QolCommandTicket g_ticket = 0;
bool g_fail_save_write = false;

QolResult save_slot_state(int32_t, int32_t* out) { *out = 0; return ok(); }
QolResult save_slot_exists(int32_t, int32_t* out) { *out = 1; return ok(); }
QolResult save_load_data(int32_t, void** out, int32_t* size) {
    *out = nullptr;
    *size = 0;
    return ok();
}
QolResult save_is_ok(int32_t* out) { *out = 1; return ok(); }
QolResult save_current_slot(int32_t* out) { *out = 1; return ok(); }

QolResult save_write(int32_t kind, int32_t, int32_t, uint32_t attempt, char* out,
                     uint32_t capacity, uint32_t* out_size) {
    if (g_fail_save_write) {
        const char* body = "{\"ok\":false,\"error\":\"sidecar: io_error\"}";
        const uint32_t size = static_cast<uint32_t>(std::strlen(body));
        std::memcpy(out, body, size);
        *out_size = size;
        return error(kQolErrInternal, "write failed", 12);
    }
    if (kind != kQolSaveWriteSave && attempt == 0) {
        *out_size = 0;
        return error(kQolErrNotReady, "world pending", 13);
    }
    const char* body = "{\"ok\":true,\"state\":{\"hp\":1}}";
    const uint32_t size = static_cast<uint32_t>(std::strlen(body));
    if (capacity < size) return error(kQolErrInvalidArgument, "tiny", 4);
    std::memcpy(out, body, size);
    *out_size = size;
    return ok();
}

QolResult post_typed(uint32_t, const void*, uint32_t, QolCommandTicket* out) {
    *out = ++g_ticket;
    return ok();
}
QolResult register_result(uint32_t, QolTypedResultCommandHandler handler, void*) {
    g_result_handler = handler;
    return ok();
}
QolResult unregister_result(uint32_t) {
    g_result_handler = nullptr;
    return ok();
}
uint32_t current_attempt() { return g_attempt; }
QolResult defer_result() { ++g_deferred; return ok(); }

int32_t is_main_thread() { return 1; }
QolResult verify_owner(QolOwnerToken token) {
    return token == 1 ? ok() : error(kQolErrBlocked, "invalid token", 13);
}
QolResult ensure_owner_slot(QolOwnerToken, int32_t) { return ok(); }
QolResult read_owner_section(QolOwnerToken, int32_t, const char*, uint8_t*, uint64_t,
                              uint64_t*, uint16_t*) { return error(kQolErrNotFound, "missing", 7); }
QolResult write_owner_section(QolOwnerToken, int32_t, const char*, uint16_t,
                               const uint8_t*, uint64_t) { return ok(); }

QolResult dispatch(uint32_t route, const char* path, const char* body, char* output,
                   uint32_t capacity, uint32_t* output_size) {
    HttpReq request{HttpMethod::kPost, path, static_cast<uint32_t>(std::strlen(path)),
                    reinterpret_cast<const uint8_t*>(body),
                    static_cast<uint32_t>(std::strlen(body)), nullptr, 0, nullptr, 0};
    HttpRes response{reinterpret_cast<uint8_t*>(output), capacity, 0, 0};
    const auto& descriptor = qol::features::core_save::plugin_descriptor();
    const QolResult result = descriptor.http_queries[route].handler(
        &request, &response, descriptor.http_queries[route].ctx);
    *output_size = response.body_size;
    return result;
}

}  // namespace

int main() {
    const auto& descriptor = qol::features::core_save::plugin_descriptor();
    CHECK(std::strcmp(descriptor.id, "core.save") == 0);
    CHECK(std::strcmp(descriptor.version, "0.1.0") == 0);
    CHECK(descriptor.requires_count == 0 && descriptor.optional_count == 0);
    CHECK(descriptor.provides_count == 2 && descriptor.provides != nullptr);
    CHECK(std::strcmp(descriptor.provides[0].id, qol::contracts::kSaveFactsCapabilityId) == 0);
    CHECK(descriptor.provides[0].version.major == 1 && descriptor.provides[0].version.minor == 0);
    CHECK(descriptor.provides[0].iface != nullptr);
    CHECK(std::strcmp(descriptor.provides[1].id, kQolSlotDataCapabilityId) == 0);
    CHECK(descriptor.provides[1].version.major == 1 && descriptor.provides[1].version.minor == 0);
    CHECK(descriptor.provides[1].iface != nullptr);
    CHECK(descriptor.port_count == 2 && descriptor.ports != nullptr);
    CHECK(descriptor.ports[0].id == qol::contracts::QolPortId::QOL_PORT_SAVE);
    CHECK(descriptor.ports[1].id == qol::contracts::QolPortId::QOL_PORT_SAVE_WRITE);
    // 写面（阶段 5 票据契约）由同域 owner 声明式提供 3 条 POST。
    CHECK(descriptor.http_query_count == 3 && descriptor.http_queries != nullptr);

    const auto* api = static_cast<const qol::contracts::SaveFactsApi*>(descriptor.provides[0].iface);
    CHECK(api->slot_state != nullptr);
    CHECK(api->slot_exists != nullptr);
    CHECK(api->slot_hero_index != nullptr);
    CHECK(api->hero_level != nullptr);
    CHECK(api->hero_class != nullptr);
    CHECK(api->load_data != nullptr);
    CHECK(api->hub_key != nullptr);
    CHECK(api->map_id != nullptr);
    CHECK(api->current_slot != nullptr);
    CHECK(api->is_ok != nullptr);

    QolSavePortReadApi save_read{};
    save_read.slot_state = &save_slot_state;
    save_read.slot_exists = &save_slot_exists;
    save_read.load_data = &save_load_data;
    save_read.is_ok = &save_is_ok;
    save_read.current_slot = &save_current_slot;
    const QolSavePort save_port{kQolPortSaveVersion, save_read};
    const QolSaveWritePort save_write_port{kQolPortSaveWriteVersion, {&save_write}};
    const void* ports[] = {&save_port, &save_write_port};
    QolHostSdk sdk{};
    sdk.abi = {sizeof(QolHostSdk), kHostSdkMajor, kHostSdkMinor};
    sdk.commands.post_typed = &post_typed;
    sdk.ticket_result.register_typed_result_handler = &register_result;
    sdk.ticket_result.unregister_typed_result_handler = &unregister_result;
    sdk.ticket_result.current_attempt = &current_attempt;
    sdk.ticket_result.defer_current_result = &defer_result;
    const QolOwnerStoreApi owner_store{{sizeof(QolOwnerStoreApi), 1, 0}, &ensure_owner_slot,
        nullptr, &read_owner_section, &write_owner_section, nullptr, nullptr};
    sdk.owner_verify_token = &verify_owner;
    sdk.owner_store = &owner_store;
    QolGameHost game{};
    game.thread.is_main_thread = &is_main_thread;
    PluginContext context{};
    context.struct_size = sizeof(PluginContext);
    context.sdk_major = kHostSdkMajor;
    context.sdk_minor = kHostSdkMinor;
    context.sdk = &sdk;
    context.ports = ports;
    context.port_count = 2;
    context.game = &game;
    context.owner_token = 1;
    CHECK(descriptor.install(context) == 0);

    char output[4096]{};
    uint32_t output_size = 0;
    CHECK(dispatch(0, "/api/system/save", "{}", output, sizeof(output),
                   &output_size).code == kQolOk);
    CHECK(std::string(output, output_size) == "{\"ok\":true,\"ticket\":1}");
    const TestSaveWritePayload save_payload{kQolSaveWriteSave, -1, -1};
    CHECK(g_result_handler(nullptr, reinterpret_cast<const uint8_t*>(&save_payload),
                           sizeof(save_payload), output, sizeof(output), &output_size));
    CHECK(std::string(output, output_size) ==
          "{\"ok\":true,\"slot\":1,\"sidecar\":\"ensured\"}");
    g_fail_save_write = true;
    CHECK(!g_result_handler(nullptr, reinterpret_cast<const uint8_t*>(&save_payload),
                            sizeof(save_payload), output, sizeof(output), &output_size));
    CHECK(std::string(output, output_size) ==
          "{\"ok\":false,\"error\":\"sidecar: io_error\"}");
    g_fail_save_write = false;

    const auto check_deferred_route = [&](uint32_t route, int32_t kind, int32_t slot,
                                          const char* path, const char* body,
                                          const char* expected) {
        CHECK(dispatch(route, path, body, output, sizeof(output), &output_size).code == kQolOk);
        const TestSaveWritePayload payload{kind, slot, 0};
        g_attempt = 0;
        CHECK(g_result_handler(nullptr, reinterpret_cast<const uint8_t*>(&payload),
                               sizeof(payload), output, sizeof(output), &output_size));
        CHECK(g_deferred > 0);
        g_attempt = 1;
        CHECK(g_result_handler(nullptr, reinterpret_cast<const uint8_t*>(&payload),
                               sizeof(payload), output, sizeof(output), &output_size));
        CHECK(std::string(output, output_size) == expected);
    };
    check_deferred_route(1, kQolSaveWriteEnterSlot, 2,
                         "/api/system/enter_slot", "{\"slot\":2}",
                         "{\"ok\":true,\"slot\":2,\"entered\":true}");
    check_deferred_route(2, kQolSaveWriteCreateSlot, 0,
                         "/api/system/create_slot", "{\"slot\":0,\"class_idx\":1}",
                         "{\"ok\":true,\"slot\":0,\"created\":true}");

    CHECK(dispatch(1, "/api/system/enter_slot", "{\"slot\":3}",
                   output, sizeof(output), &output_size).code == kQolErrInvalidArgument);
    CHECK(std::string(output, output_size) ==
          "{\"ok\":false,\"error\":\"slot required (0-2)\"}");
    CHECK(dispatch(2, "/api/system/create_slot", "{\"slot\":1}", output, sizeof(output),
                   &output_size).code == kQolErrInvalidArgument);
    CHECK(std::string(output, output_size) ==
          "{\"ok\":false,\"error\":\"class_idx required (0-5)\"}");
    descriptor.deactivate(context);
    CHECK(g_fail == 0);
    std::printf("save_facts_plugin: pass=%d fail=%d\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
