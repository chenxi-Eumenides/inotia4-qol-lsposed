// character_facts HTTP dispatch 接缝 host 测试：
// 覆盖 15 条 /api/character/* GET query handler 的注册、快照发布前的 not-ready、发布后的
// 逐路由响应（含 leader/status 的多源组合与 404 语义）、非 world 态的统一 error 体、
// owner 注销后的 handler 停用。
//
// 真实 registry/dispatch（host/http_registry.cpp、host/http_dispatch.cpp）参与链接，
// 游戏侧只以 fake QolCharacterHttpPort/QolCharacterPort 注入；富化拼装在 legacy
// game_system.cpp，本测试只验证快照/切片/组合接缝。

#include "contracts/game_ports/character_http_port.h"
#include "contracts/game_ports/character_port.h"
#include "contracts/game_ports/character_state_port.h"
#include "contracts/game_ports/character_write_ops_port.h"
#include "contracts/game_ports/character_write_port.h"
#include "contracts/game_ports/ports.h"
#include "contracts/host_sdk.h"
#include "contracts/http.h"
#include "contracts/result.h"
#include "host/http_dispatch.h"
#include "host/http_registry.h"
#include "plugins/core.character/plugin.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <string>

static int g_pass = 0;
static int g_fail = 0;

#define CHECK(cond) do { \
    if (cond) ++g_pass; \
    else { ++g_fail; std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } \
} while (0)

namespace {

using qol::contracts::QolCharacterHttpJsonKind;
using qol::contracts::QolCharacterHttpPort;
using qol::contracts::QolCharacterHttpPortReadApi;

QolResult write_json(const char* text, char* out, uint32_t capacity, uint32_t* out_size) {
    if (out_size != nullptr) *out_size = 0;
    const uint32_t size = static_cast<uint32_t>(std::strlen(text));
    if (out == nullptr || out_size == nullptr || size >= capacity) {
        return qol::contracts::error(kQolErrBlocked, "tiny buffer", 11);
    }
    std::memcpy(out, text, size);
    out[size] = '\0';
    *out_size = size;
    return qol::contracts::ok();
}

constexpr char kPlayer[] = "{\"money\":100,\"leader_slot\":1,\"party_count\":3}";
constexpr char kParty[] =
    "[{\"name\":\"凯恩\",\"class_name\":\"黑暗骑士\",\"level\":10,\"hp\":50,\"max_hp\":60,"
    "\"mp\":5,\"max_mp\":6,\"exp\":1,\"exp_next\":2,\"status_point\":3,"
    "\"equipment\":[null,{\"slot\":1,\"name\":\"钢铁 剑\"}],\"type\":0,\"class_idx\":0,"
    "\"name_id\":7,\"stats\":{\"0\":1}},null]";
constexpr char kSkills[] =
    "[{\"role\":0,\"skills\":[{\"action_id\":10,\"level\":1,\"skill_name\":\"火球\"}],"
    "\"unlock_bitmap\":0,\"active_skill_id\":-1,\"skill_points\":4},null]";
constexpr char kMercenaries[] =
    "[{\"slot\":0,\"type\":1,\"name\":\"A\"},{\"slot\":2,\"type\":1,\"name\":\"B\"}]";
constexpr char kMember0[] =
    "{\"name\":\"凯恩\",\"class_name\":\"黑暗骑士\",\"level\":10,\"hp\":50,\"max_hp\":60,"
    "\"mp\":5,\"max_mp\":6,\"exp\":1,\"exp_next\":2,\"status_point\":3,"
    "\"equipment\":[null,{\"slot\":1,\"name\":\"钢铁 剑\"}],\"type\":0,\"class_idx\":0,"
    "\"name_id\":7,\"stats\":{\"0\":1}}";
constexpr char kNotInGame[] = "{\"error\":\"not in game\"}";

bool g_not_in_game = false;
const char* g_player_json = kPlayer;
qol::contracts::QolTypedResultCommandHandler g_result_handler = nullptr;
uint32_t g_post_handler_id = 0;
uint8_t g_post_payload[qol::contracts::kTypedCommandPayloadCapacity]{};
uint32_t g_post_payload_size = 0;

struct CharWritePayloadMirror {
    int32_t kind;
    int32_t i0;
    int32_t i1;
    int32_t i2;
    int64_t i64;
    int32_t point_count;
    int32_t point_idx[8];
    int32_t point_amt[8];
};

// typed 原语 fake：记录 owner 分派到哪个原语与入参，并按 g_op_fail_at 返回失败。
enum class FakeOp : int32_t {
    kNone,
    kSetHp,
    kSetMp,
    kSetLevel,
    kSetExperience,
    kSetAttr,
    kSetStatusPoint,
    kPartySwap,
    kSetAutoAttack,
    kCastSkill,
    kAttackTarget,
    kSwitchPlayer,
    kStopCombat,
    kLearnAction,
    kAddStat,
    kStatReset,
    kSkillReset,
    kPartyInclude,
    kPartyExclude,
    kPartyDischarge,
    kPartyWithdraw,
};

FakeOp g_last_op = FakeOp::kNone;
int32_t g_op_a = 0;
int32_t g_op_b = 0;
int32_t g_op_c = 0;
int64_t g_op_i64 = 0;
int g_op_calls = 0;
int g_op_fail_at = 0;
const char* g_op_error = "port failed";

QolResult fake_op_result(qol::contracts::QolCharacterWriteOpResult* out) {
    ++g_op_calls;
    if (g_op_fail_at != g_op_calls) return qol::contracts::ok();
    const uint32_t length = static_cast<uint32_t>(std::strlen(g_op_error));
    if (out != nullptr && length < sizeof(out->error_message)) {
        std::memcpy(out->error_message, g_op_error, length + 1);
        out->error_message_len = length;
    }
    return qol::contracts::error(kQolErrInternal, g_op_error, length);
}

#define FAKE_OP1(name, id)                                                        \
    QolResult name(int32_t a, qol::contracts::QolCharacterWriteOpResult* out) {   \
        g_last_op = FakeOp::id; g_op_a = a; return fake_op_result(out);           \
    }
#define FAKE_OP2(name, id)                                                        \
    QolResult name(int32_t a, int32_t b,                                          \
                   qol::contracts::QolCharacterWriteOpResult* out) {              \
        g_last_op = FakeOp::id; g_op_a = a; g_op_b = b; return fake_op_result(out);\
    }
#define FAKE_OP3(name, id)                                                        \
    QolResult name(int32_t a, int32_t b, int32_t c,                               \
                   qol::contracts::QolCharacterWriteOpResult* out) {              \
        g_last_op = FakeOp::id; g_op_a = a; g_op_b = b; g_op_c = c;               \
        return fake_op_result(out);                                               \
    }

QolResult fake_set_exp(int32_t a, int64_t exp, qol::contracts::QolCharacterWriteOpResult* out) {
    g_last_op = FakeOp::kSetExperience;
    g_op_a = a;
    g_op_i64 = exp;
    return fake_op_result(out);
}
FAKE_OP2(fake_set_hp, kSetHp)
FAKE_OP2(fake_set_mp, kSetMp)
FAKE_OP3(fake_set_level, kSetLevel)
FAKE_OP3(fake_set_attr, kSetAttr)
FAKE_OP2(fake_set_status_point, kSetStatusPoint)
FAKE_OP2(fake_party_swap, kPartySwap)
FAKE_OP2(fake_set_auto_attack, kSetAutoAttack)
FAKE_OP2(fake_cast_skill, kCastSkill)
FAKE_OP2(fake_attack_target, kAttackTarget)
FAKE_OP1(fake_switch_player, kSwitchPlayer)
FAKE_OP1(fake_stop_combat, kStopCombat)
FAKE_OP3(fake_learn_action, kLearnAction)
FAKE_OP2(fake_add_stat, kAddStat)
FAKE_OP1(fake_stat_reset, kStatReset)
FAKE_OP1(fake_skill_reset, kSkillReset)
FAKE_OP1(fake_party_include, kPartyInclude)
FAKE_OP1(fake_party_exclude, kPartyExclude)
FAKE_OP1(fake_party_discharge, kPartyDischarge)
FAKE_OP2(fake_party_withdraw, kPartyWithdraw)

#undef FAKE_OP1
#undef FAKE_OP2
#undef FAKE_OP3

const qol::contracts::QolCharacterWriteOpsPort kFakeWriteOpsPort{
    qol::contracts::kQolPortCharacterWriteOpsVersion,
    &fake_set_hp,
    &fake_set_mp,
    &fake_set_level,
    &fake_set_exp,
    &fake_set_attr,
    &fake_set_status_point,
    &fake_party_swap,
    &fake_set_auto_attack,
    &fake_cast_skill,
    &fake_attack_target,
    &fake_switch_player,
    &fake_stop_combat,
    &fake_learn_action,
    &fake_add_stat,
    &fake_stat_reset,
    &fake_skill_reset,
    &fake_party_include,
    &fake_party_exclude,
    &fake_party_discharge,
    &fake_party_withdraw,
};

QolResult fake_character_json(int32_t kind, char* out, uint32_t capacity, uint32_t* out_size) {
    if (g_not_in_game) return write_json(kNotInGame, out, capacity, out_size);
    switch (static_cast<QolCharacterHttpJsonKind>(kind)) {
        case QolCharacterHttpJsonKind::kPlayer:
            return write_json(g_player_json, out, capacity, out_size);
        case QolCharacterHttpJsonKind::kParty:
            return write_json(kParty, out, capacity, out_size);
        case QolCharacterHttpJsonKind::kSkills:
            return write_json(kSkills, out, capacity, out_size);
        case QolCharacterHttpJsonKind::kMercenaries:
            return write_json(kMercenaries, out, capacity, out_size);
    }
    return qol::contracts::error(kQolErrInvalidArgument, "bad kind", 8);
}

QolResult fake_type(const void*, int32_t* out_type) {
    if (out_type == nullptr) return qol::contracts::error(kQolErrInvalidArgument, "bad", 3);
    *out_type = 0;
    return qol::contracts::ok();
}

QolResult fake_active(const void*, int32_t* out_active) {
    if (out_active == nullptr) return qol::contracts::error(kQolErrInvalidArgument, "bad", 3);
    *out_active = 1;
    return qol::contracts::ok();
}

const QolCharacterHttpPortReadApi kFakeHttpRead{&fake_character_json};
const QolCharacterHttpPort kFakeHttpPort{qol::contracts::kQolPortCharacterHttpVersion,
                                         kFakeHttpRead};
const qol::contracts::QolCharacterPort kFakeCharacterPort{
    qol::contracts::kQolPortCharacterVersion, {&fake_type, &fake_active}};

QolResult fake_state_party(qol::contracts::QolCharacterStateParty* out) {
    if (out == nullptr) return qol::contracts::error(kQolErrInvalidArgument, "bad out", 7);
    *out = {};
    return qol::contracts::ok();
}
QolResult fake_state_player(qol::contracts::QolCharacterStatePlayer* out) {
    if (out == nullptr) return qol::contracts::error(kQolErrInvalidArgument, "bad out", 7);
    *out = {};
    return qol::contracts::ok();
}
QolResult fake_state_skills(qol::contracts::QolCharacterStateSkills* out) {
    if (out == nullptr) return qol::contracts::error(kQolErrInvalidArgument, "bad out", 7);
    *out = {};
    return qol::contracts::ok();
}
const qol::contracts::QolCharacterStatePort kFakeStatePort{
    qol::contracts::kQolPortCharacterStateVersion,
    {&fake_state_party, &fake_state_player, &fake_state_skills}};

const void* const kPortArray[] = {&kFakeCharacterPort, &kFakeHttpPort, &kFakeWriteOpsPort,
                                  &kFakeStatePort};

struct FakeScheduler {
    qol::contracts::QolFrameHandler handler = nullptr;
    void* ctx = nullptr;
    int32_t interval = 0;
};

FakeScheduler g_scheduler;

QolResult fake_log_write(int32_t, int32_t, const char*, const char*, uint32_t) {
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
    if (ticket == nullptr || payload == nullptr || size > sizeof(g_post_payload)) {
        return qol::contracts::error(kQolErrInvalidArgument, "bad", 3);
    }
    g_post_handler_id = handler_id;
    g_post_payload_size = size;
    std::memcpy(g_post_payload, payload, size);
    *ticket = 11;
    return qol::contracts::ok();
}

QolResult fake_scheduler_add(int32_t, qol::contracts::QolFrameHandler handler, void* ctx,
                             int32_t interval, int32_t, uint64_t* out_id) {
    g_scheduler.handler = handler;
    g_scheduler.ctx = ctx;
    g_scheduler.interval = interval;
    *out_id = 1;
    return qol::contracts::ok();
}

QolResult fake_scheduler_remove(uint64_t) {
    g_scheduler.handler = nullptr;
    g_scheduler.ctx = nullptr;
    return qol::contracts::ok();
}

qol::contracts::QolHostSdk make_sdk() {
    qol::contracts::QolHostSdk sdk{};
    sdk.abi = {sizeof(qol::contracts::QolHostSdk), qol::contracts::kHostSdkMajor,
               qol::contracts::kHostSdkMinor};
    sdk.log = {&fake_log_write, nullptr};
    sdk.scheduler = {&fake_scheduler_add, &fake_scheduler_remove};
    sdk.http = qol::host::http_registry_api();
    sdk.http_dispatch = qol::host::http_dispatch_api();
    sdk.commands.post_typed = &fake_post_typed;
    sdk.ticket_result.register_typed_result_handler = &fake_register_result;
    sdk.ticket_result.unregister_typed_result_handler = &fake_unregister_result;
    return sdk;
}

const qol::contracts::HttpRoute kRoutes[] = {
    {qol::contracts::HttpMethod::kGet, "/api/character/mercenary", "core.character", 0, 1},
    {qol::contracts::HttpMethod::kGet, "/api/character/mercenary/list", "core.character", 0, 1},
    {qol::contracts::HttpMethod::kGet, "/api/character/mercenary/{slot}", "core.character", 0, 1},
    {qol::contracts::HttpMethod::kGet, "/api/character/party", "core.character", 0, 1},
    {qol::contracts::HttpMethod::kGet, "/api/character/party/count", "core.character", 0, 1},
    {qol::contracts::HttpMethod::kGet, "/api/character/leader", "core.character", 0, 1},
    {qol::contracts::HttpMethod::kGet, "/api/character/party/{slot}", "core.character", 0, 1},
    {qol::contracts::HttpMethod::kGet, "/api/character/party/{slot}/id", "core.character", 0, 1},
    {qol::contracts::HttpMethod::kGet, "/api/character/party/{slot}/name", "core.character", 0, 1},
    {qol::contracts::HttpMethod::kGet, "/api/character/party/{slot}/level", "core.character", 0, 1},
    {qol::contracts::HttpMethod::kGet, "/api/character/party/{slot}/status", "core.character", 0, 1},
    {qol::contracts::HttpMethod::kGet, "/api/character/party/{slot}/stats", "core.character", 0, 1},
    {qol::contracts::HttpMethod::kGet, "/api/character/party/{slot}/equipment", "core.character", 0,
     1},
    {qol::contracts::HttpMethod::kGet, "/api/character/party/{slot}/equipment/{equip_slot}",
     "core.character", 0, 1},
    {qol::contracts::HttpMethod::kGet, "/api/character/party/{slot}/skills", "core.character", 0,
     1},
    {qol::contracts::HttpMethod::kPost, "/api/op/character/{role}/experience", "core.character", 1, 1},
    {qol::contracts::HttpMethod::kPost, "/api/op/character/{role}/hp", "core.character", 1, 1},
    {qol::contracts::HttpMethod::kPost, "/api/op/character/{role}/level", "core.character", 1, 1},
    {qol::contracts::HttpMethod::kPost, "/api/op/character/{role}/mp", "core.character", 1, 1},
    {qol::contracts::HttpMethod::kPost, "/api/op/character/{role}/set_attr", "core.character", 1, 1},
    {qol::contracts::HttpMethod::kPost, "/api/op/character/{role}/status-point", "core.character", 1, 1},
    {qol::contracts::HttpMethod::kPost, "/api/op/party/swap", "core.character", 1, 1},
    {qol::contracts::HttpMethod::kPost, "/api/character/combat/{role}/attack_target", "core.character", 0, 1},
    {qol::contracts::HttpMethod::kPost, "/api/character/combat/{role}/cast_skill", "core.character", 0, 1},
    {qol::contracts::HttpMethod::kPost, "/api/character/combat/{role}/set_auto_attack", "core.character", 0, 1},
    {qol::contracts::HttpMethod::kPost, "/api/character/combat/{role}/stop_combat", "core.character", 0, 1},
    {qol::contracts::HttpMethod::kPost, "/api/character/combat/switch_player", "core.character", 0, 1},
    {qol::contracts::HttpMethod::kPost, "/api/character/grow/add_skill", "core.character", 0, 1},
    {qol::contracts::HttpMethod::kPost, "/api/character/grow/reset_skill", "core.character", 0, 1},
    {qol::contracts::HttpMethod::kPost, "/api/character/grow/reset_stat", "core.character", 0, 1},
    {qol::contracts::HttpMethod::kPost, "/api/character/grow/{role}/add_stat", "core.character", 0, 1},
    {qol::contracts::HttpMethod::kPost, "/api/character/party/include", "core.character", 0, 1},
    {qol::contracts::HttpMethod::kPost, "/api/character/party/exclude", "core.character", 0, 1},
    {qol::contracts::HttpMethod::kPost, "/api/character/party/discharge", "core.character", 0, 1},
    {qol::contracts::HttpMethod::kPost, "/api/character/party/withdraw", "core.character", 0, 1},
};

qol::contracts::PluginContext make_context(const qol::contracts::QolHostSdk* sdk) {
    qol::contracts::PluginContext ctx{};
    ctx.struct_size = sizeof(qol::contracts::PluginContext);
    ctx.sdk_major = qol::contracts::kHostSdkMajor;
    ctx.sdk_minor = qol::contracts::kHostSdkMinor;
    ctx.sdk = sdk;
    ctx.game = nullptr;
    ctx.plugin_id = "core.character";
    ctx.ports = kPortArray;
    ctx.port_count = 4;
    return ctx;
}

void register_frozen_routes() {
    for (const auto& route : kRoutes) {
        CHECK(qol::host::http_registry_api().register_route(&route).ok == kQolOk);
    }
}

std::string dispatch_get(const char* path, uint16_t* status = nullptr) {
    std::string path_value(path);
    char storage[8192]{};
    qol::contracts::HttpReq request{
        qol::contracts::HttpMethod::kGet,
        path_value.c_str(),
        static_cast<uint32_t>(path_value.size()),
        nullptr,
        0,
        nullptr,
        0,
        nullptr,
        0,
    };
    qol::contracts::HttpRes response{reinterpret_cast<uint8_t*>(storage), sizeof(storage), 0, 0};
    (void)qol::host::http_dispatch_api().dispatch(&request, &response);
    if (status != nullptr) *status = response.status;
    return std::string(storage, response.body_size);
}

std::string dispatch_post(const char* path, const char* body,
                          const qol::contracts::HttpPathParam* params, uint32_t param_count,
                          uint16_t* status = nullptr) {
    const std::string path_value(path);
    const uint32_t body_size = body == nullptr ? 0 : static_cast<uint32_t>(std::strlen(body));
    char storage[8192]{};
    qol::contracts::HttpReq request{
        qol::contracts::HttpMethod::kPost, path_value.c_str(),
        static_cast<uint32_t>(path_value.size()),
        reinterpret_cast<const uint8_t*>(body), body_size, nullptr, 0, params, param_count,
    };
    qol::contracts::HttpRes response{reinterpret_cast<uint8_t*>(storage), sizeof(storage), 0, 0};
    (void)qol::host::http_dispatch_api().dispatch(&request, &response);
    if (status != nullptr) *status = response.status;
    return std::string(storage, response.body_size);
}

std::string run_post_result(bool* succeeded = nullptr) {
    char storage[1024]{};
    uint32_t size = 0;
    const bool result = g_result_handler != nullptr &&
                        g_result_handler(nullptr, g_post_payload, g_post_payload_size, storage,
                                         sizeof(storage), &size);
    if (succeeded != nullptr) *succeeded = result;
    return std::string(storage, size);
}

void test_write_value_semantics() {
    uint16_t status = 0;
    g_player_json = "{\"party_count\":2,\"party_count\":\"3\",\"leader_slot\":0}";
    CHECK(g_scheduler.handler(105, g_scheduler.ctx) != 0);
    CHECK(dispatch_get("/api/character/party/count", &status) == "{\"count\":3}");
    CHECK(status == 200);
    g_player_json = kPlayer;
    CHECK(g_scheduler.handler(110, g_scheduler.ctx) != 0);

    CHECK(dispatch_post("/api/op/party/swap", "{\"a\":0,\"a\":\"2\",\"b\":\"1\"}",
                        nullptr, 0, &status) == "{\"ok\":true,\"ticket\":11}");
    CharWritePayloadMirror payload{};
    std::memcpy(&payload, g_post_payload, sizeof(payload));
    CHECK(payload.kind == static_cast<int32_t>(qol::contracts::QolCharacterWriteKind::kPartySwap));
    CHECK(payload.i0 == 2 && payload.i1 == 1);
    bool success = false;
    g_op_calls = 0;
    g_op_fail_at = 0;
    g_last_op = FakeOp::kNone;
    CHECK(run_post_result(&success) == "{\"ok\":true}" && success);
    CHECK(g_last_op == FakeOp::kPartySwap && g_op_a == 2 && g_op_b == 1);
}

void test_add_stat_result_semantics() {
    const qol::contracts::HttpPathParam role{"role", 4, "0", 1};
    uint16_t status = 0;
    CHECK(dispatch_post("/api/character/grow/0/add_stat", "{\"attrs\":{\"strength\":3.7}}",
                        &role, 1, &status) == "{\"ok\":true,\"ticket\":11}");
    CharWritePayloadMirror payload{};
    std::memcpy(&payload, g_post_payload, sizeof(payload));
    CHECK(payload.point_count == 1 && payload.point_idx[0] == 0 && payload.point_amt[0] == 3);
    g_op_calls = 0;
    g_op_fail_at = 0;
    g_last_op = FakeOp::kNone;
    bool success = false;
    CHECK(run_post_result(&success) ==
              "{\"ok\":true,\"applied\":[{\"attr\":0},{\"attr\":0},{\"attr\":0}]}" &&
          success);
    CHECK(g_op_calls == 3);
    CHECK(g_last_op == FakeOp::kAddStat && g_op_a == 0 && g_op_b == 0);

    g_op_calls = 0;
    g_op_fail_at = 1;
    CHECK(dispatch_post("/api/character/grow/0/add_stat", "{\"attrs\":{\"strength\":3.7}}",
                        &role, 1, &status) == "{\"ok\":true,\"ticket\":11}");
    CHECK(run_post_result(&success) == "{\"ok\":false,\"error\":\"port failed\"}" && !success);
    CHECK(g_op_calls == 1);
    g_op_fail_at = 0;
}

void test_set_attr_result_semantics() {
    const qol::contracts::HttpPathParam role{"role", 4, "0", 1};
    uint16_t status = 0;
    CHECK(dispatch_post("/api/op/character/0/set_attr",
                        "{\"stats\":{\"strength\":5,\"agility\":6,\"strength\":\"7\"}}",
                        &role, 1,
                        &status) == "{\"ok\":true,\"ticket\":11}");
    g_op_calls = 0;
    g_op_fail_at = 0;
    g_last_op = FakeOp::kNone;
    bool success = false;
    CHECK(run_post_result(&success) ==
              "{\"ok\":true,\"set\":[{\"attr\":0,\"value\":7},{\"attr\":1,\"value\":6}]}" &&
          success);
    CHECK(g_op_calls == 2 && g_last_op == FakeOp::kSetAttr && g_op_a == 0 && g_op_b == 1 &&
          g_op_c == 6);

    g_op_calls = 0;
    g_op_fail_at = 2;
    CHECK(dispatch_post("/api/op/character/0/set_attr",
                        "{\"stats\":{\"strength\":7,\"agility\":8}}", &role, 1,
                        &status) == "{\"ok\":true,\"ticket\":11}");
    CHECK(run_post_result(&success) == "{\"ok\":false,\"error\":\"set attr 1 failed\"}" &&
          !success);
    CHECK(g_op_calls == 2 && g_op_a == 0 && g_op_b == 1 && g_op_c == 8);
    g_op_fail_at = 0;
}

void test_no_body_and_boolean_semantics() {
    uint16_t status = 0;
    bool success = false;
    constexpr char kPlayerState[] =
        "{\"money\":0,\"map_id\":0,\"x\":0,\"y\":0,\"active_quest\":0,\"leader_slot\":0,"
        "\"party_count\":0}";
    const char* reset_paths[] = {"/api/character/grow/reset_stat",
                                 "/api/character/grow/reset_skill"};
    const qol::contracts::QolCharacterWriteKind expected_kinds[] = {
        qol::contracts::QolCharacterWriteKind::kStatReset,
        qol::contracts::QolCharacterWriteKind::kSkillReset,
    };
    const FakeOp expected_ops[] = {FakeOp::kStatReset, FakeOp::kSkillReset};
    for (size_t i = 0; i < 2; ++i) {
        CHECK(dispatch_post(reset_paths[i], nullptr, nullptr, 0, &status) ==
              "{\"ok\":true,\"ticket\":11}");
        CharWritePayloadMirror payload{};
        std::memcpy(&payload, g_post_payload, sizeof(payload));
        CHECK(payload.kind == static_cast<int32_t>(expected_kinds[i]) && payload.i0 == 0);
        g_op_calls = 0;
        g_last_op = FakeOp::kNone;
        CHECK(run_post_result(&success) ==
                  std::string("{\"ok\":true,\"state\":") + kPlayerState + "}" &&
              success);
        CHECK(g_last_op == expected_ops[i]);
    }

    CHECK(dispatch_post("/api/character/combat/switch_player",
                        "{\"slot\":0,\"slot\":\"1\"}", nullptr, 0, &status) ==
          "{\"ok\":true,\"ticket\":11}");
    CharWritePayloadMirror switch_payload{};
    std::memcpy(&switch_payload, g_post_payload, sizeof(switch_payload));
    CHECK(switch_payload.kind == static_cast<int32_t>(
                                     qol::contracts::QolCharacterWriteKind::kSwitchPlayer));
    CHECK(switch_payload.i0 == 1);

    const qol::contracts::HttpPathParam role{"role", 4, "1", 1};
    const char* values[] = {"TRUE", "FaLsE", "anything"};
    const int expected[] = {1, 0, 0};
    for (size_t i = 0; i < 3; ++i) {
        const std::string body = std::string("{\"on\":\"") + values[i] + "\"}";
        CHECK(dispatch_post("/api/character/combat/1/set_auto_attack", body.c_str(), &role, 1,
                            &status) == "{\"ok\":true,\"ticket\":11}");
        CharWritePayloadMirror payload{};
        std::memcpy(&payload, g_post_payload, sizeof(payload));
        CHECK(payload.kind == static_cast<int32_t>(
                                  qol::contracts::QolCharacterWriteKind::kSetAutoAttack));
        CHECK(payload.i0 == 1 && payload.i1 == expected[i]);
    }
}

void test_android_number_semantics() {
    uint16_t status = 0;
    const char* slot_values[] = {"1.7", "1e0", " 2 ", "+2"};
    const int expected_slots[] = {1, 1, 2, 2};
    for (size_t i = 0; i < 4; ++i) {
        const std::string body = std::string("{\"slot\":\"") + slot_values[i] + "\"}";
        CHECK(dispatch_post("/api/character/combat/switch_player", body.c_str(), nullptr, 0,
                            &status) == "{\"ok\":true,\"ticket\":11}");
        CharWritePayloadMirror payload{};
        std::memcpy(&payload, g_post_payload, sizeof(payload));
        CHECK(payload.i0 == expected_slots[i]);
    }

    CHECK(dispatch_post("/api/character/combat/switch_player", "{\"slot\":2.7}", nullptr, 0,
                        &status) == "{\"ok\":true,\"ticket\":11}");
    CharWritePayloadMirror float_payload{};
    std::memcpy(&float_payload, g_post_payload, sizeof(float_payload));
    CHECK(float_payload.i0 == 2);

    const char* invalid_values[] = {"abc", "3.7.9", "1e", ""};
    for (const char* invalid : invalid_values) {
        const std::string body = std::string("{\"slot\":\"") + invalid + "\"}";
        CHECK(dispatch_post("/api/character/combat/switch_player", body.c_str(), nullptr, 0,
                            &status) == "{\"ok\":false,\"error\":\"slot required (0-2)\"}");
        CHECK(status == 400);
    }

    CHECK(dispatch_post("/api/character/grow/add_skill",
                        "{\"action_id\":\"Infinity\"}", nullptr, 0, &status) ==
          "{\"ok\":true,\"ticket\":11}");
    CharWritePayloadMirror infinity_payload{};
    std::memcpy(&infinity_payload, g_post_payload, sizeof(infinity_payload));
    CHECK(infinity_payload.i1 == std::numeric_limits<int32_t>::max());
    CHECK(dispatch_post("/api/character/grow/add_skill", "{\"action_id\":\"NaN\"}", nullptr,
                        0, &status) == "{\"ok\":true,\"ticket\":11}");
    CharWritePayloadMirror nan_payload{};
    std::memcpy(&nan_payload, g_post_payload, sizeof(nan_payload));
    CHECK(nan_payload.i1 == 0);
    CHECK(dispatch_post("/api/character/grow/add_skill",
                        "{\"action_id\":\"1e100\"}", nullptr, 0, &status) ==
          "{\"ok\":true,\"ticket\":11}");
    std::memcpy(&infinity_payload, g_post_payload, sizeof(infinity_payload));
    CHECK(infinity_payload.i1 == std::numeric_limits<int32_t>::max());

    const qol::contracts::HttpPathParam role{"role", 4, "0", 1};
    CHECK(dispatch_post("/api/op/character/0/experience",
                        "{\"exp\":\"Infinity\"}", &role, 1, &status) ==
          "{\"ok\":true,\"ticket\":11}");
    CharWritePayloadMirror long_payload{};
    std::memcpy(&long_payload, g_post_payload, sizeof(long_payload));
    CHECK(long_payload.i64 == std::numeric_limits<int64_t>::max());
    CHECK(dispatch_post("/api/op/character/0/experience", "{\"exp\":\"1e100\"}", &role,
                        1, &status) == "{\"ok\":true,\"ticket\":11}");
    std::memcpy(&long_payload, g_post_payload, sizeof(long_payload));
    CHECK(long_payload.i64 == std::numeric_limits<int64_t>::max());
    CHECK(dispatch_post("/api/op/character/0/experience", "{\"exp\":\"1e2\"}", &role, 1,
                        &status) == "{\"ok\":true,\"ticket\":11}");
    std::memcpy(&long_payload, g_post_payload, sizeof(long_payload));
    CHECK(long_payload.i64 == 100);
    CHECK(dispatch_post("/api/op/character/0/experience", "{\"exp\":\"-Infinity\"}", &role,
                        1, &status) == "{\"ok\":false,\"error\":\"exp required\"}");
    CHECK(status == 400);
}

void test_write_kind_primitive_mapping() {
    struct MappingCase {
        qol::contracts::QolCharacterWriteKind kind;
        FakeOp op;
        int32_t i0;
        int32_t i1;
        int32_t i2;
        int64_t i64;
        int32_t point_count;
        int32_t point_idx;
        int32_t point_amt;
        int expected_calls;
    };
    const MappingCase cases[] = {
        {qol::contracts::QolCharacterWriteKind::kSetHp, FakeOp::kSetHp, 1, 50, 0, 0, 0, 0, 0, 1},
        {qol::contracts::QolCharacterWriteKind::kSetMp, FakeOp::kSetMp, 1, 10, 0, 0, 0, 0, 0, 1},
        {qol::contracts::QolCharacterWriteKind::kSetLevel, FakeOp::kSetLevel, 1, 12, 1, 0, 0, 0, 0, 1},
        {qol::contracts::QolCharacterWriteKind::kSetExperience, FakeOp::kSetExperience, 1, 0, 0, 100, 0, 0, 0, 1},
        {qol::contracts::QolCharacterWriteKind::kSetAttr, FakeOp::kSetAttr, 1, 0, 0, 0, 1, 2, 9, 1},
        {qol::contracts::QolCharacterWriteKind::kSetStatusPoint, FakeOp::kSetStatusPoint, 1, 5, 0, 0, 0, 0, 0, 1},
        {qol::contracts::QolCharacterWriteKind::kPartySwap, FakeOp::kPartySwap, 0, 1, 0, 0, 0, 0, 0, 1},
        {qol::contracts::QolCharacterWriteKind::kSetAutoAttack, FakeOp::kSetAutoAttack, 1, 1, 0, 0, 0, 0, 0, 1},
        {qol::contracts::QolCharacterWriteKind::kCastSkill, FakeOp::kCastSkill, 1, 7, 0, 0, 0, 0, 0, 1},
        {qol::contracts::QolCharacterWriteKind::kAttackTarget, FakeOp::kAttackTarget, 1, 3, 0, 0, 0, 0, 0, 1},
        {qol::contracts::QolCharacterWriteKind::kSwitchPlayer, FakeOp::kSwitchPlayer, 2, 0, 0, 0, 0, 0, 0, 1},
        {qol::contracts::QolCharacterWriteKind::kStopCombat, FakeOp::kStopCombat, 1, 0, 0, 0, 0, 0, 0, 1},
        {qol::contracts::QolCharacterWriteKind::kLearnAction, FakeOp::kLearnAction, 0, 7, 2, 0, 0, 0, 0, 1},
        {qol::contracts::QolCharacterWriteKind::kAddStat, FakeOp::kAddStat, 1, 0, 0, 0, 1, 2, 3, 3},
        {qol::contracts::QolCharacterWriteKind::kStatReset, FakeOp::kStatReset, 1, 0, 0, 0, 0, 0, 0, 1},
        {qol::contracts::QolCharacterWriteKind::kSkillReset, FakeOp::kSkillReset, 1, 0, 0, 0, 0, 0, 0, 1},
        {qol::contracts::QolCharacterWriteKind::kPartyInclude, FakeOp::kPartyInclude, 4, 0, 0, 0, 0, 0, 0, 1},
        {qol::contracts::QolCharacterWriteKind::kPartyExclude, FakeOp::kPartyExclude, 4, 0, 0, 0, 0, 0, 0, 1},
        {qol::contracts::QolCharacterWriteKind::kPartyDischarge, FakeOp::kPartyDischarge, 4, 0, 0, 0, 0, 0, 0, 1},
        {qol::contracts::QolCharacterWriteKind::kPartyWithdraw, FakeOp::kPartyWithdraw, 4, 2, 0, 0, 0, 0, 0, 1},
    };
    for (const auto& test : cases) {
        CharWritePayloadMirror payload{};
        payload.kind = static_cast<int32_t>(test.kind);
        payload.i0 = test.i0;
        payload.i1 = test.i1;
        payload.i2 = test.i2;
        payload.i64 = test.i64;
        payload.point_count = test.point_count;
        if (test.point_count > 0) {
            payload.point_idx[0] = test.point_idx;
            payload.point_amt[0] = test.point_amt;
        }
        g_op_calls = 0;
        g_op_fail_at = 0;
        g_last_op = FakeOp::kNone;
        char storage[1024]{};
        uint32_t size = 0;
        const bool ok = g_result_handler != nullptr &&
                        g_result_handler(nullptr, reinterpret_cast<const uint8_t*>(&payload),
                                         sizeof(payload), storage, sizeof(storage), &size);
        CHECK(ok);
        CHECK(g_last_op == test.op);
        CHECK(g_op_calls == test.expected_calls);
    }
}

void test_lifecycle_and_responses() {
    qol::host::http_dispatch_reset_for_test();
    qol::host::http_registry_reset_for_test();
    qol::host::http_dispatch_set_op_enabled(true);
    g_scheduler = FakeScheduler{};
    g_not_in_game = false;
    register_frozen_routes();

    const qol::contracts::QolHostSdk sdk = make_sdk();
    qol::contracts::PluginContext ctx = make_context(&sdk);
    const auto& descriptor = qol::features::core_character::plugin_descriptor();

    CHECK(descriptor.install(ctx) == 0);
    descriptor.on_enabled(ctx);
    CHECK(qol::host::http_dispatch_register_queries(descriptor.http_queries,
                                                     descriptor.http_query_count).ok == kQolOk);
    CHECK(g_scheduler.handler != nullptr);
    CHECK(g_scheduler.interval > 1);

    uint16_t status = 0;
    (void)dispatch_get("/api/character/party", &status);
    CHECK(status == 503);

    CHECK(g_scheduler.handler(100, g_scheduler.ctx) != 0);

    CHECK(dispatch_get("/api/character/party") == kParty);
    CHECK(dispatch_get("/api/character/party/count") == "{\"count\":3}");
    CHECK(dispatch_get("/api/character/party/0") == kMember0);
    CHECK(dispatch_get("/api/character/party/0/id") == "{\"id\":0,\"id_name\":\"黑暗骑士\"}");
    CHECK(dispatch_get("/api/character/party/0/name") == "{\"name\":\"凯恩\"}");
    CHECK(dispatch_get("/api/character/party/0/level") == "{\"level\":10}");
    CHECK(dispatch_get("/api/character/party/0/status") ==
          "{\"status\":{\"hp\":50,\"max_hp\":60,\"mp\":5,\"max_mp\":6,\"exp\":1,\"exp_next\":2,"
          "\"attribute_points\":3,\"skill_points\":4}}");
    CHECK(dispatch_get("/api/character/party/0/stats") == "{\"stats\":{\"0\":1}}");
    CHECK(dispatch_get("/api/character/party/0/equipment") ==
          "{\"equipment\":[null,{\"slot\":1,\"name\":\"钢铁 剑\"}]}");
    CHECK(dispatch_get("/api/character/party/0/equipment/1") == "{\"slot\":1,\"name\":\"钢铁 剑\"}");
    CHECK(dispatch_get("/api/character/party/0/skills") ==
          "{\"role\":0,\"skills\":[{\"action_id\":10,\"level\":1,\"skill_name\":\"火球\"}],"
          "\"unlock_bitmap\":0,\"active_skill_id\":-1,\"skill_points\":4}");

    CHECK(dispatch_get("/api/character/mercenary") == kMercenaries);
    CHECK(dispatch_get("/api/character/mercenary/list") == "{\"slots\":[0,2]}");
    CHECK(dispatch_get("/api/character/mercenary/2") == "{\"slot\":2,\"type\":1,\"name\":\"B\"}");

    // 404 语义：null 成员、越界成员、equipment 空槽、leader 指向 null、无匹配佣兵槽。
    (void)dispatch_get("/api/character/party/1", &status);
    CHECK(status == 404);
    CHECK(dispatch_get("/api/character/party/1/stats") == "{\"stats\":null}");
    (void)dispatch_get("/api/character/party/9", &status);
    CHECK(status == 404);
    (void)dispatch_get("/api/character/party/0/equipment/0", &status);
    CHECK(status == 404);
    (void)dispatch_get("/api/character/leader", &status);
    CHECK(status == 404);
    (void)dispatch_get("/api/character/mercenary/5", &status);
    CHECK(status == 404);

    test_write_value_semantics();
    test_add_stat_result_semantics();
    test_set_attr_result_semantics();
    test_no_body_and_boolean_semantics();
    test_android_number_semantics();
    test_write_kind_primitive_mapping();

    // 非 world 态：15 条统一 200 + {"error":"not in game"}。
    g_not_in_game = true;
    CHECK(g_scheduler.handler(115, g_scheduler.ctx) != 0);
    CHECK(dispatch_get("/api/character/party", &status) == kNotInGame);
    CHECK(status == 200);
    CHECK(dispatch_get("/api/character/mercenary", &status) == kNotInGame);
    CHECK(status == 200);
    CHECK(dispatch_get("/api/character/party/count", &status) == kNotInGame);
    CHECK(status == 200);

    // 注销 owner：handler 进入墓碑态，调度器任务撤销，请求返回明确停用错误。
    CHECK(qol::host::http_dispatch_api().unregister_owner("core.character").ok == kQolOk);
    descriptor.on_disabled(ctx);
    CHECK(g_scheduler.handler == nullptr);
    (void)dispatch_get("/api/character/party", &status);
    CHECK(status == 503);

    // 恢复：路由元数据保留，重新绑定后需等待首轮快照。
    g_not_in_game = false;
    descriptor.on_enabled(ctx);
    CHECK(qol::host::http_dispatch_register_queries(descriptor.http_queries,
                                                     descriptor.http_query_count).ok == kQolOk);
    CHECK(g_scheduler.handler != nullptr);
    (void)dispatch_get("/api/character/party", &status);
    CHECK(status == 503);
    CHECK(g_scheduler.handler(130, g_scheduler.ctx) != 0);
    CHECK(dispatch_get("/api/character/party") == kParty);

    descriptor.deactivate(ctx);
}

void test_missing_route_rejected() {
    qol::host::http_dispatch_reset_for_test();
    qol::host::http_registry_reset_for_test();
    g_scheduler = FakeScheduler{};
    g_not_in_game = false;

    const qol::contracts::QolHostSdk sdk = make_sdk();
    qol::contracts::PluginContext ctx = make_context(&sdk);
    const auto& descriptor = qol::features::core_character::plugin_descriptor();
    CHECK(descriptor.install(ctx) == 0);
    descriptor.on_enabled(ctx);
    CHECK(qol::host::http_dispatch_register_queries(descriptor.http_queries,
                                                     descriptor.http_query_count).code ==
          kQolErrNotFound);
    descriptor.on_disabled(ctx);
    CHECK(g_scheduler.handler == nullptr);
    descriptor.deactivate(ctx);
}

}  // namespace

int main() {
    test_lifecycle_and_responses();
    test_missing_route_rejected();
    std::printf("character_facts_dispatch: pass=%d fail=%d\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
