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
#include "contracts/item_state.h"
#include "contracts/game_ports/ports.h"
#include "contracts/host_sdk.h"
#include "contracts/http.h"
#include "contracts/result.h"
#include "host/http_dispatch.h"
#include "host/http_registry.h"
#include "plugins/core.character/plugin.h"
#include "test_snapshot_sdk.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <string>
#include <vector>

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
constexpr char kMember1[] =
    "{\"name\":\"佣兵B\",\"class_name\":\"忍者\",\"level\":8,\"hp\":30,"
    "\"max_hp\":40,\"mp\":4,\"max_mp\":5,\"exp\":2,\"exp_next\":3,"
    "\"status_point\":1,\"equipment\":[null],\"type\":1,\"class_idx\":1,"
    "\"name_id\":8,\"stats\":{\"0\":2}}";
constexpr char kMember2[] =
    "{\"name\":\"佣兵C\",\"class_name\":\"黑魔导\",\"level\":6,\"hp\":20,"
    "\"max_hp\":25,\"mp\":8,\"max_mp\":10,\"exp\":3,\"exp_next\":4,"
    "\"status_point\":0,\"equipment\":[null],\"type\":1,\"class_idx\":2,"
    "\"name_id\":9,\"stats\":{\"0\":3}}";

bool g_not_in_game = false;
int32_t g_failed_http_kind = -1;
int g_snapshot_warn_count = 0;
std::string g_player_json = kPlayer;
std::string g_party_json = kParty;
std::string g_skills_json = kSkills;
std::string g_party_after_write_json;
std::string g_skills_after_write_json;
std::string g_player_after_write_json;
const char* g_inventory_before_write_json =
    "{\"bags\":[{\"bag\":0,\"items\":[],\"capacity\":16,\"slot_count\":0}]}";
const char* g_inventory_after_write_json =
    "{\"bags\":[{\"bag\":0,\"items\":[{\"slot\":2,\"name\":\"Potion\","
    "\"category\":10,\"count\":1,\"item_type\":\"consumable\",\"need_level\":0,"
    "\"rarity\":0,\"rarity_tier\":\"白\",\"base\":{\"damage\":0,\"defense\":0,"
    "\"magic_rate\":0},\"bonus\":[],\"gem\":{\"total_slots\":0,\"slots\":[]},"
    "\"chaos\":{\"is_chaos\":false,\"level\":0,\"rate\":100},"
    "\"enchant\":{\"id\":0,\"level\":0,\"effect\":null}}],\"capacity\":16,"
    "\"slot_count\":1}]}";
bool g_item_state_available = true;
bool g_item_state_parse_failure = false;
int g_item_state_snapshot_reads = 0;
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
int32_t g_main_stats[3][5] = {{10, 20, 30, 40, 50}, {1, 2, 3, 4, 5}, {6, 7, 8, 9, 10}};
int32_t g_status_points[3] = {12, 3, 2};
int32_t g_skill_points[3] = {4, 2, 1};
int32_t g_leader_slot = 0;
int32_t g_auto_attack[3] = {};
int32_t g_active_skill[3] = {-1, -1, -1};

QolResult fake_op_result(qol::contracts::QolCharacterWriteOpResult* out) {
    ++g_op_calls;
    if (g_op_fail_at != g_op_calls) {
        switch (g_last_op) {
            case FakeOp::kAddStat:
                if (g_op_a >= 0 && g_op_a < 3 && g_op_b >= 0 && g_op_b < 5) {
                    ++g_main_stats[g_op_a][g_op_b];
                    --g_status_points[g_op_a];
                }
                break;
            case FakeOp::kStatReset:
                if (g_op_a >= 0 && g_op_a < 3) {
                    for (int32_t i = 0; i < 5; ++i) g_main_stats[g_op_a][i] = i + 1;
                    g_status_points[g_op_a] = 18;
                }
                break;
            case FakeOp::kSkillReset:
            case FakeOp::kLearnAction:
                if (!g_skills_after_write_json.empty()) g_skills_json = g_skills_after_write_json;
                if (g_last_op == FakeOp::kSkillReset && g_op_a >= 0 && g_op_a < 3) {
                    g_skill_points[g_op_a] = 11;
                }
                break;
            case FakeOp::kSwitchPlayer:
                g_leader_slot = g_op_a;
                if (!g_player_after_write_json.empty()) g_player_json = g_player_after_write_json;
                break;
            case FakeOp::kPartyInclude:
            case FakeOp::kPartyExclude:
            case FakeOp::kPartyDischarge:
                if (!g_party_after_write_json.empty()) g_party_json = g_party_after_write_json;
                break;
            case FakeOp::kPartyWithdraw:
                if (!g_party_after_write_json.empty()) g_party_json = g_party_after_write_json;
                break;
            case FakeOp::kAttackTarget:
                if (!g_party_after_write_json.empty()) g_party_json = g_party_after_write_json;
                break;
            case FakeOp::kSetAutoAttack:
                if (g_op_a >= 0 && g_op_a < 3) g_auto_attack[g_op_a] = g_op_b;
                break;
            case FakeOp::kCastSkill:
                if (g_op_a >= 0 && g_op_a < 3) g_active_skill[g_op_a] = g_op_b;
                break;
            default:
                break;
        }
        return qol::contracts::ok();
    }
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
    if (kind == g_failed_http_kind) {
        return qol::contracts::error(kQolErrNotReady, "fake read failure", 17);
    }
    if (g_not_in_game) return write_json(kNotInGame, out, capacity, out_size);
    switch (static_cast<QolCharacterHttpJsonKind>(kind)) {
        case QolCharacterHttpJsonKind::kPlayer:
            return write_json(g_player_json.c_str(), out, capacity, out_size);
        case QolCharacterHttpJsonKind::kParty:
            return write_json(g_party_json.c_str(), out, capacity, out_size);
        case QolCharacterHttpJsonKind::kSkills:
            return write_json(g_skills_json.c_str(), out, capacity, out_size);
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

bool g_state_party_fails = false;
QolResult fake_state_party(qol::contracts::QolCharacterStateParty* out) {
    if (out == nullptr) return qol::contracts::error(kQolErrInvalidArgument, "bad out", 7);
    if (g_state_party_fails) {
        return qol::contracts::error(kQolErrNotReady, "state unavailable", 17);
    }
    *out = {};
    for (uint32_t role = 0; role < qol::contracts::kQolCharacterStatePartySize; ++role) {
        out->member_present[role] = 1;
        out->members[role].fields = qol::contracts::kQolCharacterStateHasStatusPoint;
        out->members[role].status_point = g_status_points[role];
        for (uint32_t stat = 0; stat < qol::contracts::kQolCharacterStateMainStatCount; ++stat) {
            out->members[role].main_stats[stat] = g_main_stats[role][stat];
        }
    }
    return qol::contracts::ok();
}
QolResult fake_state_player(qol::contracts::QolCharacterStatePlayer* out) {
    if (out == nullptr) return qol::contracts::error(kQolErrInvalidArgument, "bad out", 7);
    *out = {};
    out->leader_slot = g_leader_slot;
    return qol::contracts::ok();
}
QolResult fake_state_skills(qol::contracts::QolCharacterStateSkills* out) {
    if (out == nullptr) return qol::contracts::error(kQolErrInvalidArgument, "bad out", 7);
    *out = {};
    for (uint32_t role = 0; role < qol::contracts::kQolCharacterStatePartySize; ++role) {
        out->member_present[role] = 1;
        out->members[role].skill_points = g_skill_points[role];
    }
    return qol::contracts::ok();
}
const qol::contracts::QolCharacterStatePort kFakeStatePort{
    qol::contracts::kQolPortCharacterStateVersion,
    {&fake_state_party, &fake_state_player, &fake_state_skills}};

QolResult fake_item_state_json(int32_t kind, char* out, uint32_t capacity, uint32_t* out_size) {
    if (out_size != nullptr) *out_size = 0;
    if (kind != static_cast<int32_t>(qol::contracts::QolItemStateKind::kEnrichedInventory)) {
        return qol::contracts::error(kQolErrInvalidArgument, "bad item state kind", 19);
    }
    ++g_item_state_snapshot_reads;
    const char* json = g_item_state_parse_failure
                           ? "not-json"
                           : (g_item_state_snapshot_reads % 2 == 1
                                  ? g_inventory_before_write_json
                                  : g_inventory_after_write_json);
    const uint32_t size = static_cast<uint32_t>(std::strlen(json));
    if (out == nullptr || out_size == nullptr || size > capacity) {
        return qol::contracts::error(kQolErrBlocked, "item state buffer small", 23);
    }
    std::memcpy(out, json, size);
    *out_size = size;
    return qol::contracts::ok();
}
const qol::contracts::ItemStateApi kFakeItemStateApi{&fake_item_state_json};

QolResult fake_capability_require(const char* id, ::QolVersionRange,
                                  const void** out_iface) {
    if (id == nullptr || out_iface == nullptr) {
        return qol::contracts::error(kQolErrInvalidArgument, "bad capability", 15);
    }
    if (std::strcmp(id, qol::contracts::kItemStateCapabilityId) == 0 &&
        g_item_state_available) {
        *out_iface = &kFakeItemStateApi;
        return qol::contracts::ok();
    }
    return qol::contracts::error(kQolErrNotFound, "capability missing", 18);
}

const void* const kPortArray[] = {&kFakeCharacterPort, &kFakeHttpPort, &kFakeWriteOpsPort,
                                  &kFakeStatePort};

struct FakeScheduler {
    qol::contracts::QolFrameHandler handler = nullptr;
    void* ctx = nullptr;
    int32_t interval = 0;
};

FakeScheduler g_scheduler;

QolResult fake_log_write(int32_t level, int32_t, const char*, const char*, uint32_t) {
    if (level == 2) ++g_snapshot_warn_count;
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
    sdk.capabilities.require = &fake_capability_require;
    sdk.http = qol::host::http_registry_api();
    sdk.http_dispatch = qol::host::http_dispatch_api();
    sdk.commands.post_typed = &fake_post_typed;
    sdk.ticket_result.register_typed_result_handler = &fake_register_result;
    sdk.ticket_result.unregister_typed_result_handler = &fake_unregister_result;
    test_snapshot_sdk::attach(&sdk);
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

std::string dispatch_get_template(const char* path, const qol::contracts::HttpPathParam* params,
                                 uint32_t param_count, uint16_t* status) {
    const std::string path_value(path);
    char storage[8192]{};
    qol::contracts::HttpReq request{
        qol::contracts::HttpMethod::kGet, path_value.c_str(),
        static_cast<uint32_t>(path_value.size()), nullptr, 0, nullptr, 0, params, param_count,
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
    g_main_stats[0][0] = 10;
    g_main_stats[0][2] = 30;
    g_status_points[0] = 12;
    uint16_t status = 0;
    CHECK(dispatch_post("/api/character/grow/0/add_stat", "{\"attrs\":{\"strength\":1.5}}",
                        &role, 1, &status) ==
          "{\"ok\":false,\"error\":\"bad value for strength (must be integer)\"}");
    CHECK(status == 400);
    CHECK(dispatch_post("/api/character/grow/0/add_stat", "{\"attrs\":{\"strength\":2.0}}",
                        &role, 1, &status) == "{\"ok\":true,\"ticket\":11}");
    CharWritePayloadMirror integer_payload{};
    std::memcpy(&integer_payload, g_post_payload, sizeof(integer_payload));
    CHECK(integer_payload.point_count == 1 && integer_payload.point_amt[0] == 2);
    CHECK(dispatch_post("/api/character/grow/0/add_stat", "{\"attrs\":{\"strength\":3}}",
                        &role, 1, &status) == "{\"ok\":true,\"ticket\":11}");
    CharWritePayloadMirror payload{};
    std::memcpy(&payload, g_post_payload, sizeof(payload));
    CHECK(payload.point_count == 1 && payload.point_idx[0] == 0 && payload.point_amt[0] == 3);
    const uint32_t accepted_payload_size = g_post_payload_size;
    const uint32_t accepted_handler = g_post_handler_id;
    CHECK(dispatch_post("/api/character/grow/0/add_stat", "{\"attrs\":{\"strength\":4}}",
                        &role, 1, &status) == "{\"ok\":false,\"error\":\"no status point\"}");
    CHECK(status == 400 && g_post_payload_size == accepted_payload_size &&
          g_post_handler_id == accepted_handler);
    g_op_calls = 0;
    g_op_fail_at = 0;
    g_last_op = FakeOp::kNone;
    bool success = false;
    CHECK(run_post_result(&success) ==
              "{\"ok\":true,\"applied\":[{\"attr\":0},{\"attr\":0},{\"attr\":0}],"
              "\"role\":{\"role\":0,\"main_stats\":[{\"attr\":0,\"value\":13}],"
              "\"status_point\":9}}" &&
          success);
    CHECK(g_op_calls == 3);
    CHECK(g_last_op == FakeOp::kAddStat && g_op_a == 0 && g_op_b == 0);

    const qol::contracts::HttpPathParam overflow_role{"role", 4, "99999999999999999999", 20};
    const qol::contracts::HttpPathParam negative_role{"role", 4, "-1", 2};
    const uint32_t old_post_size = g_post_payload_size;
    const uint32_t old_post_handler = g_post_handler_id;
    CHECK(dispatch_post("/api/op/character/{role}/experience", "{\"exp\":1}",
                        &overflow_role, 1, &status) ==
          "{\"ok\":false,\"error\":\"role required\"}");
    CHECK(status == 400 && g_post_payload_size == old_post_size &&
          g_post_handler_id == old_post_handler);
    CHECK(dispatch_post("/api/op/character/{role}/experience", "{\"exp\":1}",
                        &negative_role, 1, &status) ==
          "{\"ok\":false,\"error\":\"role required\"}");
    CHECK(status == 400 && g_post_payload_size == old_post_size &&
          g_post_handler_id == old_post_handler);

    const qol::contracts::HttpPathParam overflow_slot{"slot", 4, "99999999999999999999", 20};
    CHECK(dispatch_get_template("/api/character/mercenary/{slot}", &overflow_slot, 1,
                                &status) == "{\"ok\":false,\"error\":\"bad request\"}");
    CHECK(status == 400);
    const qol::contracts::HttpPathParam negative_slot{"slot", 4, "-1", 2};
    CHECK(dispatch_get_template("/api/character/mercenary/{slot}", &negative_slot, 1,
                                &status) == "{\"ok\":false,\"error\":\"bad request\"}");
    CHECK(status == 400);

    g_op_calls = 0;
    g_op_fail_at = 1;
    CHECK(dispatch_post("/api/character/grow/0/add_stat", "{\"attrs\":{\"strength\":3}}",
                        &role, 1, &status) == "{\"ok\":true,\"ticket\":11}");
    CHECK(run_post_result(&success) == "{\"ok\":false,\"error\":\"port failed\"}" && !success);
    CHECK(g_op_calls == 1);
    g_op_fail_at = 0;

    g_status_points[0] = 12;
    CHECK(dispatch_post("/api/character/grow/0/add_stat", "{\"attrs\":{\"strength\":3}}",
                        &role, 1, &status) == "{\"ok\":true,\"ticket\":11}");
    g_status_points[0] = 2;
    g_op_calls = 0;
    CHECK(run_post_result(&success) == "{\"ok\":false,\"error\":\"no status point\"}" &&
          !success);
    CHECK(g_op_calls == 0);
    g_status_points[0] = 12;
}

void test_operation_specific_result_goldens() {
    uint16_t status = 0;
    bool succeeded = false;
    const auto run = [&]() { return run_post_result(&succeeded); };

    g_skills_json = kSkills;
    g_skills_after_write_json =
        "[{\"role\":0,\"skills\":[{\"action_id\":7,\"level\":2,"
        "\"max_level\":10,\"skill_name\":\"火球\"}],\"unlock_bitmap\":9,"
        "\"active_skill_id\":-1,\"skill_points\":3},null,null]";
    CHECK(dispatch_post("/api/character/grow/add_skill", "{\"action_id\":7}", nullptr, 0,
                        &status) == "{\"ok\":true,\"ticket\":11}");
    g_op_calls = 0;
    CHECK(run() ==
          "{\"ok\":true,\"skills\":[{\"action_id\":7,\"level\":2}],"
          "\"skill_points\":3}" && succeeded);

    g_main_stats[1][0] = 10;
    g_main_stats[1][2] = 30;
    g_status_points[1] = 8;
    const qol::contracts::HttpPathParam role1{"role", 4, "1", 1};
    CHECK(dispatch_post("/api/character/grow/1/add_stat",
                        "{\"attrs\":{\"strength\":2,\"vitality\":1}}", &role1, 1,
                        &status) == "{\"ok\":true,\"ticket\":11}");
    g_op_calls = 0;
    CHECK(run() ==
          "{\"ok\":true,\"applied\":[{\"attr\":0},{\"attr\":0},{\"attr\":2}],"
          "\"role\":{\"role\":1,\"main_stats\":[{\"attr\":0,\"value\":12},"
          "{\"attr\":2,\"value\":31}],\"status_point\":5}}" && succeeded);

    g_player_after_write_json = "{\"money\":100,\"leader_slot\":2,\"party_count\":3}";
    CHECK(dispatch_post("/api/character/combat/switch_player", "{\"slot\":2}", nullptr, 0,
                        &status) == "{\"ok\":true,\"ticket\":11}");
    g_op_calls = 0;
    CHECK(run() == "{\"ok\":true,\"player\":{\"leader_slot\":2}}" && succeeded);

    g_skills_after_write_json =
        "[{\"role\":0,\"skills\":[{\"action_id\":7,\"level\":3,"
        "\"max_level\":10,\"skill_name\":\"火球\"}],\"unlock_bitmap\":9,"
        "\"active_skill_id\":-1,\"skill_points\":2},null,null]";
    const qol::contracts::HttpPathParam role0{"role", 4, "0", 1};
    CHECK(dispatch_post("/api/character/grow/reset_skill", nullptr, nullptr, 0, &status) ==
          "{\"ok\":true,\"ticket\":11}");
    g_op_calls = 0;
    CHECK(run() == "{\"ok\":true,\"skill_points\":2}" && succeeded);

    g_skills_json =
        "[null,{\"role\":1,\"skills\":[{\"action_id\":7,\"level\":3}],"
        "\"unlock_bitmap\":0,\"active_skill_id\":7,\"skill_points\":2},null]";
    CHECK(dispatch_post("/api/character/combat/1/cast_skill", "{\"action_id\":7}", &role1,
                        1, &status) == "{\"ok\":true,\"ticket\":11}");
    g_op_calls = 0;
    CHECK(run() == "{\"ok\":true,\"role\":{\"role\":1,\"active_skill_id\":7}}" &&
          succeeded);

    CHECK(dispatch_post("/api/character/combat/1/stop_combat", nullptr, &role1, 1, &status) ==
          "{\"ok\":true,\"ticket\":11}");
    g_op_calls = 0;
    CHECK(run() == "{\"ok\":true,\"role\":{\"role\":1,\"combat\":false}}" &&
          succeeded);

    const std::string confirmed_target_member =
        std::string(kMember1).substr(0, std::strlen(kMember1) - 1) +
        ",\"target_slot\":2,\"combat\":true}";
    g_party_json = "[" + std::string(kMember0) + "," + kMember1 + ",null]";
    g_party_after_write_json =
        "[" + std::string(kMember0) + "," + confirmed_target_member + ",null]";
    CHECK(dispatch_post("/api/character/combat/1/attack_target", "{\"target_slot\":2}",
                        &role1, 1, &status) == "{\"ok\":true,\"ticket\":11}");
    g_op_calls = 0;
    CHECK(run() ==
          "{\"ok\":true,\"role\":{\"role\":1,\"target_slot\":2,\"combat\":true}}" &&
          succeeded);

    g_party_after_write_json = "[" + std::string(kMember0) + "," + kMember1 + ",null]";
    CHECK(dispatch_post("/api/character/combat/1/attack_target", "{\"target_slot\":2}",
                        &role1, 1, &status) == "{\"ok\":true,\"ticket\":11}");
    g_op_calls = 0;
    CHECK(run() == "{\"ok\":true}" && succeeded);

    g_party_json = "[" + std::string(kMember0) + ",null," + kMember2 + "]";
    g_party_after_write_json = "[" + std::string(kMember0) + "," + kMember1 + ",null]";
    CHECK(dispatch_post("/api/character/party/include", "{\"mercenary_slot\":7}", nullptr,
                        0, &status) == "{\"ok\":true,\"ticket\":11}");
    g_op_calls = 0;
    CHECK(run() == "{\"ok\":true,\"party\":[{\"slot\":1,\"member\":" +
                      std::string(kMember1) + "},{\"slot\":2,\"member\":null}]}" &&
          succeeded);

    g_party_json = "[" + std::string(kMember0) + "," + kMember1 + "," + kMember2 + "]";
    g_party_after_write_json = "[" + std::string(kMember0) + ",null,null]";
    CHECK(dispatch_post("/api/character/party/exclude", "{\"mercenary_slot\":7}", nullptr,
                        0, &status) == "{\"ok\":true,\"ticket\":11}");
    g_op_calls = 0;
    CHECK(run() ==
          "{\"ok\":true,\"party\":[{\"slot\":1,\"member\":null},"
          "{\"slot\":2,\"member\":null}]}" && succeeded);

    g_party_json = "[" + std::string(kMember0) + "," + kMember1 + ",null]";
    g_party_after_write_json = "[" + std::string(kMember0) + ",null,null]";
    CHECK(dispatch_post("/api/character/party/discharge", "{\"mercenary_slot\":7}", nullptr,
                        0, &status) == "{\"ok\":true,\"ticket\":11}");
    g_op_calls = 0;
    CHECK(run() == "{\"ok\":true,\"party\":[{\"slot\":1,\"member\":null}]}" &&
          succeeded);

    g_item_state_available = true;
    g_item_state_parse_failure = false;
    g_item_state_snapshot_reads = 0;
    CHECK(dispatch_post("/api/character/party/withdraw",
                        "{\"mercenary_slot\":7,\"equip_slot\":0}", nullptr, 0, &status) ==
          "{\"ok\":true,\"ticket\":11}");
    g_op_calls = 0;
    CHECK(run() ==
          "{\"ok\":true,\"equipment\":{\"mercenary_slot\":7,\"slot\":0,"
          "\"item\":null},\"inventory\":[{\"bag\":0,\"slot\":2,\"item\":"
          "{\"slot\":2,\"name\":\"Potion\",\"category\":10,\"count\":1,"
          "\"item_type\":\"consumable\",\"need_level\":0,\"rarity\":0,"
          "\"rarity_tier\":\"白\",\"base\":{\"damage\":0,\"defense\":0,"
          "\"magic_rate\":0},\"bonus\":[],\"gem\":{\"total_slots\":0,\"slots\":[]},"
          "\"chaos\":{\"is_chaos\":false,\"level\":0,\"rate\":100},"
          "\"enchant\":{\"id\":0,\"level\":0,\"effect\":null}}}]}" && succeeded);
    CHECK(g_item_state_snapshot_reads == 2);

    g_item_state_available = false;
    g_item_state_snapshot_reads = 0;
    CHECK(dispatch_post("/api/character/party/withdraw",
                        "{\"mercenary_slot\":7,\"equip_slot\":0}", nullptr, 0, &status) ==
          "{\"ok\":true,\"ticket\":11}");
    g_op_calls = 0;
    CHECK(run() ==
          "{\"ok\":true,\"equipment\":{\"mercenary_slot\":7,\"slot\":0,"
          "\"item\":null}}" && succeeded);

    g_item_state_available = true;
    g_item_state_parse_failure = true;
    g_item_state_snapshot_reads = 0;
    CHECK(dispatch_post("/api/character/party/withdraw",
                        "{\"mercenary_slot\":7,\"equip_slot\":0}", nullptr, 0, &status) ==
          "{\"ok\":true,\"ticket\":11}");
    g_op_calls = 0;
    CHECK(run() ==
          "{\"ok\":true,\"equipment\":{\"mercenary_slot\":7,\"slot\":0,"
          "\"item\":null}}" && succeeded);
    g_item_state_parse_failure = false;
    g_party_after_write_json.clear();
    g_party_json = kParty;
    g_skills_json = kSkills;
    g_player_json = kPlayer;
}

void test_set_attr_result_semantics() {
    const qol::contracts::HttpPathParam role{"role", 4, "0", 1};
    uint16_t status = 0;
    CHECK(dispatch_post("/api/op/character/0/set_attr",
                        "{\"stats\":{\"strength\":\"1.5\"}}", &role, 1, &status) ==
          "{\"ok\":false,\"error\":\"bad value for strength (must be integer)\"}");
    CHECK(status == 400);
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
    const char* reset_paths[] = {"/api/character/grow/reset_stat",
                                  "/api/character/grow/reset_skill"};
    const qol::contracts::QolCharacterWriteKind expected_kinds[] = {
        qol::contracts::QolCharacterWriteKind::kStatReset,
        qol::contracts::QolCharacterWriteKind::kSkillReset,
    };
    const FakeOp expected_ops[] = {FakeOp::kStatReset, FakeOp::kSkillReset};
    g_main_stats[0][0] = 10;
    g_main_stats[0][1] = 20;
    g_main_stats[0][2] = 30;
    g_main_stats[0][3] = 40;
    g_main_stats[0][4] = 50;
    g_status_points[0] = 2;
    g_skills_after_write_json =
        "[{\"role\":0,\"skills\":[],\"unlock_bitmap\":0,\"active_skill_id\":-1,"
        "\"skill_points\":11},null,null]";
    for (size_t i = 0; i < 2; ++i) {
        CHECK(dispatch_post(reset_paths[i], nullptr, nullptr, 0, &status) ==
              "{\"ok\":true,\"ticket\":11}");
        CharWritePayloadMirror payload{};
        std::memcpy(&payload, g_post_payload, sizeof(payload));
        CHECK(payload.kind == static_cast<int32_t>(expected_kinds[i]) && payload.i0 == 0);
        g_op_calls = 0;
        g_last_op = FakeOp::kNone;
        const std::string expected = i == 0
            ? "{\"ok\":true,\"role\":{\"role\":0,\"main_stats\":[1,2,3,4,5],"
              "\"status_point\":18}}"
            : "{\"ok\":true,\"skill_points\":11}";
        CHECK(run_post_result(&success) == expected && success);
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
    bool result_ok = false;
    g_state_party_fails = true;
    CHECK(dispatch_post("/api/character/combat/1/set_auto_attack", "{\"on\":true}", &role,
                        1, &status) == "{\"ok\":true,\"ticket\":11}");
    CHECK(run_post_result(&result_ok) ==
          "{\"ok\":true,\"role\":{\"role\":1,\"auto_attack\":true}}" && result_ok);
    g_state_party_fails = false;
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
    const uint32_t initial_payload_size = g_post_payload_size;
    CHECK(dispatch_post("/api/op/character/0/experience",
                        "{\"exp\":\"Infinity\"}", &role, 1, &status) ==
          "{\"ok\":false,\"error\":\"exp required\"}");
    CHECK(status == 400 && g_post_payload_size == initial_payload_size);
    CharWritePayloadMirror long_payload{};
    std::memcpy(&long_payload, g_post_payload, sizeof(long_payload));
    CHECK(long_payload.i64 == 0);
    CHECK(dispatch_post("/api/op/character/0/experience", "{\"exp\":\"1e100\"}", &role,
                        1, &status) == "{\"ok\":false,\"error\":\"exp required\"}");
    CHECK(status == 400);
    std::memcpy(&long_payload, g_post_payload, sizeof(long_payload));
    CHECK(long_payload.i64 == 0);
    CHECK(dispatch_post("/api/op/character/0/experience", "{\"exp\":\"1e2\"}", &role, 1,
                        &status) == "{\"ok\":true,\"ticket\":11}");
    std::memcpy(&long_payload, g_post_payload, sizeof(long_payload));
    CHECK(long_payload.i64 == 100);
    CHECK(dispatch_post("/api/op/character/0/experience", "{\"exp\":2147483647}",
                        &role, 1, &status) == "{\"ok\":true,\"ticket\":11}");
    std::memcpy(&long_payload, g_post_payload, sizeof(long_payload));
    CHECK(long_payload.i64 == std::numeric_limits<int32_t>::max());
    const uint32_t previous_payload_size = g_post_payload_size;
    CHECK(dispatch_post("/api/op/character/0/experience", "{\"exp\":2147483648}",
                        &role, 1, &status) == "{\"ok\":false,\"error\":\"exp required\"}");
    CHECK(status == 400 && g_post_payload_size == previous_payload_size);
    CHECK(dispatch_post("/api/op/character/0/experience", "{\"exp\":-1}", &role, 1,
                        &status) == "{\"ok\":false,\"error\":\"exp required\"}");
    CHECK(status == 400);
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
    g_failed_http_kind = -1;
    g_snapshot_warn_count = 0;
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

    g_failed_http_kind = static_cast<int32_t>(QolCharacterHttpJsonKind::kParty);
    CHECK(g_scheduler.handler(115, g_scheduler.ctx) != 0);
    CHECK(dispatch_get("/api/character/party") == kParty);
    CHECK(g_snapshot_warn_count == 1);
    CHECK(g_scheduler.handler(130, g_scheduler.ctx) != 0);
    CHECK(dispatch_get("/api/character/party") == kParty);
    CHECK(g_snapshot_warn_count == 1);
    g_failed_http_kind = -1;
    CHECK(g_scheduler.handler(145, g_scheduler.ctx) != 0);
    CHECK(g_snapshot_warn_count == 1);
    g_failed_http_kind = static_cast<int32_t>(QolCharacterHttpJsonKind::kParty);
    CHECK(g_scheduler.handler(160, g_scheduler.ctx) != 0);
    CHECK(g_snapshot_warn_count == 2);
    g_failed_http_kind = -1;
    CHECK(g_scheduler.handler(175, g_scheduler.ctx) != 0);

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
    test_operation_specific_result_goldens();
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
