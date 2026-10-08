#include "contracts/save_facts.h"
#include "contracts/game_ports/ports.h"
#include "contracts/game_ports/save_port.h"
#include "contracts/game_ports/save_transaction_port.h"
#include "contracts/game_ports/save_write_port.h"
#include "contracts/host_sdk.h"
#include "contracts/http.h"
#include "contracts/result.h"
#include "contracts/runtime_hooks.h"
#include "contracts/slot_data.h"
#include "contracts/slot_operation.h"
#include "host/owner_identity.h"
#include "host/save_route.h"
#include "plugins/core.save/plugin.h"
#include "plugins/core.save/save_transition.h"

#include <cstdio>
#include <cstring>
#include <string>

static int g_pass = 0;
static int g_fail = 0;

#define CHECK(condition) do { \
    if (condition) ++g_pass; \
    else { ++g_fail; std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition); } \
} while (0)

// 测试专用 runtime_hooks 表（生产实现 host/frame_source.cpp 不在本 target）：
// thunk 缝复用宿主 dispatch 语义（按 owner token → save_route → core.save native_call）。
namespace qol::features {
using QolOwnerToken = qol::contracts::QolOwnerToken;
QolOwnerToken g_test_core_save_owner = 0;
QolOwnerToken core_save_test_owner_token() { return g_test_core_save_owner; }
}  // namespace qol::features

namespace qol::contracts {
namespace {
bool fake_begin_thunk(int32_t kind, int32_t target_slot, uint64_t* out_transition) {
    if (out_transition != nullptr) *out_transition = 0;
    QolSaveNativeCall call{};
    call.size = sizeof(call);
    call.kind = static_cast<uint32_t>(kind == 0 ? QolSaveCallKind::kResume
                                                : QolSaveCallKind::kBeginNew);
    call.target_slot = target_slot;
    call.class_index = -1;
    call.origin = static_cast<uint32_t>(QolSaveOrigin::kOrdinary);
    QolSaveDomainStatus status{};
    status.size = sizeof(status);
    // token 查询缝未注入：直接用测试持有的 owner token 派发。
    const QolResult result = qol::host::save_route::dispatch_native_call(
        qol::features::core_save_test_owner_token(), &call, &status);
    if (out_transition != nullptr) *out_transition = status.transition;
    return succeeded(result);
}
bool fake_enter_ready() {
    QolSaveNativeCall call{};
    call.size = sizeof(call);
    call.kind = static_cast<uint32_t>(QolSaveCallKind::kMaintenance);
    call.target_slot = -1;
    call.origin = static_cast<uint32_t>(QolSaveOrigin::kOrdinary);
    QolSaveDomainStatus status{};
    status.size = sizeof(status);
    const QolResult result = qol::host::save_route::dispatch_native_call(
        qol::features::core_save_test_owner_token(), &call, &status);
    if (result.code == kQolErrNotFound) return true;
    return succeeded(result);
}
const QolRuntimeHooksApi kTestHooks{
    {1, 0}, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,
    &fake_begin_thunk, &fake_enter_ready,
};
}  // namespace
const QolRuntimeHooksApi& qol_runtime_hooks() { return kTestHooks; }
}  // namespace qol::contracts

namespace {

using namespace qol::contracts;

struct TestSaveWritePayload {
    int32_t kind;
    int32_t slot;
    int32_t class_idx;
    uint64_t request_sequence;
};

QolTypedResultCommandHandler g_result_handler = nullptr;
uint32_t g_attempt = 0;
int g_deferred = 0;
QolCommandTicket g_ticket = 0;
TestSaveWritePayload g_posted_payload{};
uint64_t g_last_transition_token = 0;
uint64_t g_next_lease_id = 0;
bool g_fail_save_write = false;
bool g_fail_logic_scheduler = false;
uint64_t g_next_scheduler_task = 1;

QolResult save_slot_state(int32_t, int32_t* out) { *out = 0; return ok(); }
QolResult save_slot_exists(int32_t, int32_t* out) { *out = 1; return ok(); }
QolResult save_load_data(int32_t, void** out, int32_t* size) {
    *out = nullptr;
    *size = 0;
    return ok();
}
QolResult save_is_ok(int32_t* out) { *out = 1; return ok(); }
QolResult save_current_slot(int32_t* out) { *out = 1; return ok(); }

QolResult save_write(int32_t kind, int32_t slot, int32_t, uint32_t attempt, char* out,
                     uint32_t capacity, uint32_t* out_size) {
    if (g_fail_save_write) {
        const char* body = "{\"ok\":false,\"error\":\"sidecar: io_error\"}";
        const uint32_t size = static_cast<uint32_t>(std::strlen(body));
        std::memcpy(out, body, size);
        *out_size = size;
        return error(kQolErrInternal, "write failed", 12);
    }
    if (kind != kQolSaveWriteSave && attempt == 0) {
        const auto& hooks = qol_runtime_hooks();
        uint64_t token = 0;
        const int32_t transition_kind = kind == kQolSaveWriteEnterSlot ? 0 : 1;
        if (hooks.save_transition_begin_thunk == nullptr ||
            !hooks.save_transition_begin_thunk(transition_kind, slot, &token)) {
            const char* body = "{\"ok\":false,\"error\":\"slot transition not confirmed\"}";
            const uint32_t size = static_cast<uint32_t>(std::strlen(body));
            if (capacity < size) return error(kQolErrInvalidArgument, "tiny", 4);
            std::memcpy(out, body, size);
            *out_size = size;
            return error(kQolErrInternal, "transition begin failed", 23);
        }
        g_last_transition_token = token;
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

QolResult post_typed(uint32_t, const void* payload, uint32_t size, QolCommandTicket* out) {
    if (payload == nullptr || size != sizeof(g_posted_payload))
        return error(kQolErrInvalidArgument, "bad typed payload", 18);
    std::memcpy(&g_posted_payload, payload, size);
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
QolResult scheduler_add(int32_t point, QolFrameHandler, void*, int32_t, int32_t,
                        uint64_t* out_id) {
    if (point == kQolRuntimeFramePointLogicPre && g_fail_logic_scheduler)
        return error(kQolErrInternal, "logic scheduler failure", 23);
    *out_id = g_next_scheduler_task++;
    return ok();
}
QolResult scheduler_remove(uint64_t) { return ok(); }

int32_t is_main_thread() { return 1; }

// ---- P3c install 面：owner 身份 + save_route 授权 + 完整 ABI 缝 ----
using namespace qol::contracts;
struct OwnerIdentityAnchor { int anchor = 0; } g_owner_identity;
QolOwnerToken g_owner_token = 0;

QolResult stub_ok_result(QolOwnerToken, int32_t) { return ok(); }
QolResult stub_remove_section(QolOwnerToken, int32_t, const char*) { return ok(); }
QolResult stub_read(QolOwnerToken, int32_t, const char*, uint8_t*, uint64_t, uint64_t*,
                    uint16_t*) {
    return error(kQolErrNotFound, "missing", 7);
}
QolResult stub_write(QolOwnerToken, int32_t, const char*, uint16_t, const uint8_t*, uint64_t) {
    return ok();
}
QolResult stub_replace_sections(QolOwnerToken, int32_t, const QolStoreSection*, uint32_t,
                                const char* const*, uint32_t) {
    return ok();
}
QolResult stub_inspect(QolOwnerToken, int32_t, QolOwnerStoreSlotState* out) {
    out->size = sizeof(*out);
    out->state = static_cast<uint32_t>(QolOwnerStoreState::kAbsent);
    out->generation = 0;
    out->encoded_bytes = 0;
    out->reservation = QolOwnerStoreReservation{};
    out->reservation.size = sizeof(QolOwnerStoreReservation);
    return ok();
}
QolResult stub_preflight(QolOwnerToken, int32_t, const QolOwnerStoreCandidate*, uint32_t,
                         uint64_t* out_lengths) {
    if (out_lengths != nullptr) *out_lengths = 64;
    return ok();
}
QolResult stub_reserve(QolOwnerToken, int32_t, const QolOwnerStoreCandidate*, uint32_t,
                       QolOwnerStoreReservation* out) {
    out->size = sizeof(*out);
    return error(kQolErrNotReady, "no reservation", 16);
}
QolResult stub_release(QolOwnerToken, QolOwnerStoreReservation) { return ok(); }
QolResult stub_write_reserved(QolOwnerToken, int32_t, QolOwnerStoreReservation*, const char*,
                              uint16_t, const uint8_t*, uint64_t) {
    return ok();
}
QolResult stub_remove_reserved(QolOwnerToken, int32_t, QolOwnerStoreReservation*, const char*) {
    return ok();
}
QolResult stub_replace_reserved(QolOwnerToken, int32_t, QolOwnerStoreReservation*,
                                const QolStoreSection*, uint32_t, const char* const*, uint32_t) {
    return ok();
}
QolResult stub_set_referenced(QolOwnerToken, int32_t, QolOwnerStoreReservation*, uint32_t) {
    return ok();
}
QolResult stub_reserve_in_operation(QolOwnerToken, const QolSlotOperationLease*, int32_t,
                                    const QolOwnerStoreCandidate*, uint32_t,
                                    QolOwnerStoreReservation* out) {
    out->size = sizeof(*out);
    return error(kQolErrNotReady, "no reservation", 16);
}
QolResult stub_ensure_in_operation(QolOwnerToken, const QolSlotOperationLease*, int32_t) {
    return ok();
}
QolResult stub_reset_in_operation(QolOwnerToken, const QolSlotOperationLease*, int32_t) {
    return ok();
}

QolResult stub_acquire(void*, QolOwnerToken owner, uint32_t mask, QolSlotOperationLease* out) {
    if (out == nullptr || mask == 0) return error(kQolErrInvalidArgument, "bad lease", 9);
    out->size = sizeof(*out);
    out->slot_mask = mask;
    out->id = ++g_next_lease_id;
    out->owner = owner;
    out->revision = 1;
    out->referenced = 0;
    return ok();
}
QolResult stub_set_ref(void*, QolOwnerToken, QolSlotOperationLease*, uint32_t) { return ok(); }
QolResult stub_refresh(void*, QolOwnerToken, uint64_t, QolSlotOperationLease*) { return ok(); }
QolResult stub_release_lease(void*, QolOwnerToken, const QolSlotOperationLease*) { return ok(); }

// Adapter fake：未绑定 backup（本测试不触发原版调用面）。
QolResult adapter_read_state(void*, QolSaveAdapterState* out) {
    out->size = sizeof(*out);
    out->current_slot = 1;
    out->world_ready = 0;
    out->new_game_flag = 0;
    return ok();
}
QolResult adapter_bind_backup(void*, uint32_t, void*) { return ok(); }
QolResult adapter_call_save(void*, const QolSlotOperationLease*, int32_t,
                            QolSaveOriginalResult* out) {
    out->size = sizeof(*out);
    out->invoked = 0;
    out->result_valid = 0;
    out->original_result = 0;
    return error(kQolErrNotReady, "backup not bound", 17);
}
QolResult adapter_read_evidence(void*, const QolSlotOperationLease*, int32_t,
                                QolSaveFileEvidence*) {
    return error(kQolErrNotReady, "platform unavailable", 17);
}

// 领域回调捕获：SAVE_Save wrapper / thunk 的派发在 save_route 真表上转回 core.save。
QolResult verify_owner(QolOwnerToken token) {
    return token == g_owner_token && g_owner_token != 0
               ? ok()
               : error(kQolErrBlocked, "invalid token", 13);
}

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
    CHECK(descriptor.port_count == 3 && descriptor.ports != nullptr);
    CHECK(descriptor.ports[0].id == qol::contracts::QolPortId::QOL_PORT_SAVE);
    CHECK(descriptor.ports[1].id == qol::contracts::QolPortId::QOL_PORT_SAVE_WRITE);
    CHECK(descriptor.ports[2].id == qol::contracts::QolPortId::QOL_PORT_SAVE_TRANSACTION);
    // P3c 资源声明：SAVE_Save 完整入口 + 新档边界 + 选角取消（Required+FailClosed）。
    CHECK(descriptor.resources_count == 3 && descriptor.resources != nullptr);
    for (uint32_t i = 0; i < descriptor.resources_count; ++i) {
        const auto& claim = descriptor.resources[i];
        CHECK(claim.mechanism == qol::contracts::ResourceMechanism::kEntryHook);
        CHECK(claim.requirement == qol::contracts::ResourceRequirement::kRequired);
        CHECK(claim.failure == qol::contracts::ResourceFailure::kFailClosed);
        CHECK(claim.handler != nullptr);
    }
    CHECK(std::strcmp(descriptor.resources[0].id, "hook:SAVE_Save") == 0);
    CHECK(std::strcmp(descriptor.resources[0].symbol, "elf-export:SAVE_Save") == 0);
    CHECK(std::strcmp(descriptor.resources[1].id, "hook:SaveSlot_GoToNewGame") == 0);
    CHECK(std::strcmp(descriptor.resources[2].id, "hook:SelectCharacter_ButtonBackExe") == 0);
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

    // owner 身份：真实注册 + save_route 授权（bind 的授权面在宿主侧）。
    CHECK(qol::host::owner_register(&g_owner_identity));
    CHECK(qol::host::owner_bind_save_route(&g_owner_identity));
    CHECK(qol::host::owner_bind_slot_operation(&g_owner_identity));
    uint64_t generation = 0;
    CHECK(qol::contracts::succeeded(
        qol::host::owner_identity_api().query(&g_owner_identity, &g_owner_token, &generation)));

    QolSavePortReadApi save_read{};
    save_read.slot_state = &save_slot_state;
    save_read.slot_exists = &save_slot_exists;
    save_read.load_data = &save_load_data;
    save_read.is_ok = &save_is_ok;
    save_read.current_slot = &save_current_slot;
    const QolSavePort save_port{kQolPortSaveVersion, save_read};
    const QolSaveWritePort save_write_port{kQolPortSaveWriteVersion, {&save_write}};
    const qol::contracts::QolSaveTransactionPort save_tx_port{
        {sizeof(qol::contracts::QolSaveTransactionPort),
         qol::contracts::kQolPortSaveTransactionVersion.major,
         qol::contracts::kQolPortSaveTransactionVersion.minor},
        nullptr, &adapter_read_state, &adapter_bind_backup, &adapter_call_save, nullptr,
        &adapter_read_evidence, nullptr};
    const void* ports[] = {&save_port, &save_write_port, &save_tx_port};
    QolHostSdk sdk{};
    sdk.abi = {sizeof(QolHostSdk), kHostSdkMajor, kHostSdkMinor};
    sdk.scheduler = {&scheduler_add, &scheduler_remove};
    sdk.commands.post_typed = &post_typed;
    sdk.ticket_result.register_typed_result_handler = &register_result;
    sdk.ticket_result.unregister_typed_result_handler = &unregister_result;
    sdk.ticket_result.current_attempt = &current_attempt;
    sdk.ticket_result.defer_current_result = &defer_result;
    // 完整 ABI 尾部的最小 stub：install 只验证可装配性；行为面由 save_transition/coordinator
    // 专属测试覆盖。
    const QolOwnerStoreApi owner_store{
        {sizeof(QolOwnerStoreApi), 1, 0}, &stub_ok_result, &stub_ok_result, &stub_read,
        &stub_write, &stub_remove_section, &stub_replace_sections, &stub_inspect,
        &stub_preflight, &stub_reserve, &stub_release, &stub_write_reserved,
        &stub_remove_reserved, &stub_replace_reserved, &stub_set_referenced,
        &stub_reserve_in_operation, &stub_ensure_in_operation, &stub_reset_in_operation};
    static const qol::contracts::QolSlotOperationApi kOperation{
        {sizeof(qol::contracts::QolSlotOperationApi), 1, 0}, nullptr, &stub_acquire, &stub_set_ref,
        &stub_refresh, &stub_release_lease, nullptr, nullptr};
    sdk.owner_verify_token = &verify_owner;
    sdk.owner_store = &owner_store;
    sdk.slot_operation = &kOperation;
    sdk.save_route = &qol::host::save_route::save_route_api();
    QolGameHost game{};
    game.thread.is_main_thread = &is_main_thread;
    PluginContext context{};
    context.struct_size = sizeof(PluginContext);
    context.sdk_major = kHostSdkMajor;
    context.sdk_minor = kHostSdkMinor;
    context.sdk = &sdk;
    context.ports = ports;
    context.port_count = 3;
    context.game = &game;
    context.owner_token = g_owner_token;
    qol::features::g_test_core_save_owner = g_owner_token;
    g_fail_logic_scheduler = true;
    CHECK(descriptor.install(context) != 0);
    CHECK(g_result_handler == nullptr);  // R4：LogicPre 缺失时完整回滚 install。
    g_fail_logic_scheduler = false;
    CHECK(descriptor.install(context) == 0);
    CHECK(g_result_handler != nullptr);
    descriptor.on_enabled(context);

    char output[4096]{};
    uint32_t output_size = 0;
    CHECK(dispatch(0, "/api/system/save", "{}", output, sizeof(output),
                   &output_size).code == kQolOk);
    CHECK(std::string(output, output_size) == "{\"ok\":true,\"ticket\":1}");
    // P3c：kSave 走唯一联合协调器结算。session 未就绪（无 ready session）时事务不发起，
    // 票据失败终态携带错误体——不再回落到旧 module_save 参与者序（已退役）。
    const TestSaveWritePayload save_payload{kQolSaveWriteSave, -1, -1};
    if (g_result_handler(nullptr, reinterpret_cast<const uint8_t*>(&save_payload),
                         sizeof(save_payload), output, sizeof(output), &output_size)) {
        std::printf("DBG handler returned true body=[%s]\n",
                    std::string(output, output_size).c_str());
    }
    CHECK(!g_result_handler(nullptr, reinterpret_cast<const uint8_t*>(&save_payload),
                            sizeof(save_payload), output, sizeof(output), &output_size));
    CHECK(std::string(output, output_size).find("{\"ok\":false") == 0);

    const auto begin_http_transition = [&](uint32_t route, const char* path, const char* body) {
        CHECK(dispatch(route, path, body, output, sizeof(output), &output_size).code == kQolOk);
        const TestSaveWritePayload payload = g_posted_payload;
        CHECK(payload.request_sequence != 0);
        g_attempt = 0;
        const int deferred_before = g_deferred;
        CHECK(g_result_handler(nullptr, reinterpret_cast<const uint8_t*>(&payload),
                               sizeof(payload), output, sizeof(output), &output_size));
        CHECK(g_deferred == deferred_before + 1);
        CHECK(g_last_transition_token != 0);
        return payload;
    };
    // R3a: only the bound token reaching ready on its requested slot completes the ticket.
    const auto matching_payload = begin_http_transition(
        1, "/api/system/enter_slot", "{\"slot\":2}");
    const uint64_t matching_token = g_last_transition_token;
    CHECK(qol::features::core_save::save_transition_confirm(matching_token, 2).ok == kQolOk);
    g_attempt = 1;
    CHECK(g_result_handler(nullptr, reinterpret_cast<const uint8_t*>(&matching_payload),
                           sizeof(matching_payload), output, sizeof(output), &output_size));
    CHECK(std::string(output, output_size) == "{\"ok\":true,\"slot\":2,\"entered\":true}");

    // R3b: another completed transition cannot complete a ticket bound to a cancelled token.
    const auto cancelled_payload = begin_http_transition(
        1, "/api/system/enter_slot", "{\"slot\":2}");
    const uint64_t cancelled_token = g_last_transition_token;
    g_attempt = 1;
    const int pending_deferred_before = g_deferred;
    CHECK(g_result_handler(nullptr, reinterpret_cast<const uint8_t*>(&cancelled_payload),
                           sizeof(cancelled_payload), output, sizeof(output), &output_size));
    CHECK(g_deferred == pending_deferred_before + 1); // token 仍在进行时票据继续 pending。
    CHECK(qol::features::core_save::save_transition_cancel(cancelled_token).ok == kQolOk);
    uint64_t unrelated_token = 0;
    CHECK(qol::features::core_save::save_transition_begin(
              static_cast<uint32_t>(qol::features::core_save::TransitionKind::kResume), 0,
              &unrelated_token).ok == kQolOk);
    CHECK(qol::features::core_save::save_transition_confirm(unrelated_token, 0).ok == kQolOk);
    g_attempt = 2;
    CHECK(!g_result_handler(nullptr, reinterpret_cast<const uint8_t*>(&cancelled_payload),
                            sizeof(cancelled_payload), output, sizeof(output), &output_size));
    CHECK(std::string(output, output_size) ==
          "{\"ok\":false,\"error\":\"slot transition not confirmed\"}");

    // R3c: slot identity is checked against the ticket's transition token.
    const auto mismatched_payload = begin_http_transition(
        2, "/api/system/create_slot", "{\"slot\":1,\"class_idx\":1}");
    const uint64_t mismatched_token = g_last_transition_token;
    CHECK(qol::features::core_save::save_transition_confirm(mismatched_token, 1).ok == kQolOk);
    auto wrong_slot_payload = mismatched_payload;
    wrong_slot_payload.slot = 2;
    g_attempt = 1;
    CHECK(!g_result_handler(nullptr, reinterpret_cast<const uint8_t*>(&wrong_slot_payload),
                            sizeof(wrong_slot_payload), output, sizeof(output), &output_size));
    CHECK(std::string(output, output_size) ==
          "{\"ok\":false,\"error\":\"slot transition not confirmed\"}");

    CHECK(dispatch(1, "/api/system/enter_slot", "{\"slot\":3}",
                   output, sizeof(output), &output_size).code == kQolErrInvalidArgument);
    CHECK(std::string(output, output_size) ==
          "{\"ok\":false,\"error\":\"slot required (0-2)\"}");
    CHECK(dispatch(2, "/api/system/create_slot", "{\"slot\":1}", output, sizeof(output),
                   &output_size).code == kQolErrInvalidArgument);
    CHECK(std::string(output, output_size) ==
          "{\"ok\":false,\"error\":\"class_idx required (0-5)\"}");
    // ---- P3c：SAVE_Save 入口 wrapper（经 descriptor 资源 handler）----
    // 激活 provider（on_enabled → slot_data available）；无 session、无读档转换时
    // wrapper fail-closed 返回 0（invoked=0，不执行原版）。
    {
        const auto save_save = reinterpret_cast<int (*)()>(
            qol::features::core_save::plugin_descriptor().resources[0].handler);
        CHECK(save_save() == 0);  // session 非 ready 且无旁路凭据。
        // begin thunk（host 缝的真实现经 save_route 派发）：成功取得 lease 后取消，
        // 确认序前 maintenance 探针保持 Blocked。
        const auto& hooks = qol_runtime_hooks();
        if (hooks.save_transition_begin_thunk != nullptr &&
            hooks.save_enter_ready != nullptr) {
            uint64_t transition = 0;
            CHECK(hooks.save_transition_begin_thunk(0, 1, &transition) && transition != 0);
            CHECK(qol::features::core_save::save_transition_cancel(transition).ok == kQolOk);
            CHECK(!hooks.save_enter_ready());  // 确认序未完成：不派发旧回调/事件。
        }
        // native_call（kSave）：经真 save_route 派发回 core.save，session 未 ready 时
        // 事务失败（kFailed，不调用原版）。
        qol::contracts::QolSaveNativeCall call{};
        call.size = sizeof(call);
        call.kind = static_cast<uint32_t>(qol::contracts::QolSaveCallKind::kSave);
        call.target_slot = 1;
        call.origin = static_cast<uint32_t>(qol::contracts::QolSaveOrigin::kOrdinary);
        qol::contracts::QolSaveDomainStatus status{};
        status.size = sizeof(status);
        const QolResult routed = qol::host::save_route::dispatch_native_call(
            g_owner_token, &call, &status);
        CHECK(routed.code == kQolErrBlocked);
        CHECK(status.original.invoked == 0);
        qol::features::core_save::plugin_descriptor().on_disabled(context);
    }

    descriptor.deactivate(context);
    CHECK(g_result_handler == nullptr);
    qol::host::owner_identity_reset_for_test();
    qol::host::save_route::reset_for_test();
    CHECK(g_fail == 0);
    std::printf("save_facts_plugin: pass=%d fail=%d\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
