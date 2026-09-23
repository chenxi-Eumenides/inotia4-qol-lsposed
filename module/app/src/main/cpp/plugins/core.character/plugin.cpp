#include "plugin.h"

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>

#include "contracts/character_facts.h"
#include "contracts/game_ports/character_http_port.h"
#include "contracts/game_ports/character_port.h"
#include "contracts/host_sdk.h"
#include "contracts/http.h"
#include "json_slice.h"
#include "snapshot.h"

namespace qol::features::core_character {
namespace {

constexpr const char* kPluginId = "core.character";
constexpr const char* kPluginVersion = "0.1.0";
// 单响应体上限：party 三人（含 10 件装备富化）远小于此值，超出即放弃本轮发布。
constexpr uint32_t kPartyCapacity = 65536;
constexpr uint32_t kSkillsCapacity = 16384;
constexpr uint32_t kMercenariesCapacity = 32768;
constexpr uint32_t kPlayerCapacity = 512;
// 主线程快照节拍：party/skills JSON 构造不宜每帧执行；15 帧（≈4Hz）对只读查询足够。
constexpr int32_t kSnapshotIntervalFrames = 15;
constexpr char kNotInGameBody[] = "{\"error\":\"not in game\"}";

enum class Query : uint8_t {
    kMercenary,
    kMercenaryList,
    kMercenarySlot,
    kParty,
    kPartyCount,
    kLeader,
    kPartyMember,
    kPartyMemberId,
    kPartyMemberName,
    kPartyMemberLevel,
    kPartyMemberStatus,
    kPartyMemberStats,
    kPartyMemberEquipment,
    kPartyMemberEquip,
    kPartyMemberSkills,
};

constexpr contracts::HttpRoute kRoutes[] = {
    {contracts::HttpMethod::kGet, "/api/character/mercenary", kPluginId, 0, 1},
    {contracts::HttpMethod::kGet, "/api/character/mercenary/list", kPluginId, 0, 1},
    {contracts::HttpMethod::kGet, "/api/character/mercenary/{slot}", kPluginId, 0, 1},
    {contracts::HttpMethod::kGet, "/api/character/party", kPluginId, 0, 1},
    {contracts::HttpMethod::kGet, "/api/character/party/count", kPluginId, 0, 1},
    {contracts::HttpMethod::kGet, "/api/character/leader", kPluginId, 0, 1},
    {contracts::HttpMethod::kGet, "/api/character/party/{slot}", kPluginId, 0, 1},
    {contracts::HttpMethod::kGet, "/api/character/party/{slot}/id", kPluginId, 0, 1},
    {contracts::HttpMethod::kGet, "/api/character/party/{slot}/name", kPluginId, 0, 1},
    {contracts::HttpMethod::kGet, "/api/character/party/{slot}/level", kPluginId, 0, 1},
    {contracts::HttpMethod::kGet, "/api/character/party/{slot}/status", kPluginId, 0, 1},
    {contracts::HttpMethod::kGet, "/api/character/party/{slot}/stats", kPluginId, 0, 1},
    {contracts::HttpMethod::kGet, "/api/character/party/{slot}/equipment", kPluginId, 0, 1},
    {contracts::HttpMethod::kGet, "/api/character/party/{slot}/equipment/{equip_slot}", kPluginId, 0,
     1},
    {contracts::HttpMethod::kGet, "/api/character/party/{slot}/skills", kPluginId, 0, 1},
};

constexpr Query kQueries[] = {
    Query::kMercenary,        Query::kMercenaryList,     Query::kMercenarySlot,
    Query::kParty,            Query::kPartyCount,        Query::kLeader,
    Query::kPartyMember,      Query::kPartyMemberId,     Query::kPartyMemberName,
    Query::kPartyMemberLevel, Query::kPartyMemberStatus, Query::kPartyMemberStats,
    Query::kPartyMemberEquipment, Query::kPartyMemberEquip, Query::kPartyMemberSkills,
};

enum class SnapshotState : uint8_t { kUnavailable, kNotInGame, kReady };

struct CharacterSnapshot {
    SnapshotState state = SnapshotState::kUnavailable;
    uint32_t party_size = 0;
    char party[kPartyCapacity]{};
    uint32_t skills_size = 0;
    char skills[kSkillsCapacity]{};
    uint32_t mercenaries_size = 0;
    char mercenaries[kMercenariesCapacity]{};
    uint32_t player_size = 0;
    char player[kPlayerCapacity]{};
};

const contracts::QolHostSdk* g_sdk = nullptr;
const contracts::QolCharacterPort* g_port = nullptr;
const contracts::QolCharacterHttpPort* g_http_port = nullptr;
uint64_t g_frame_task = 0;
Snapshot<CharacterSnapshot> g_snapshot;

const contracts::QolPortRequirement kPorts[] = {
    {contracts::QolPortId::QOL_PORT_CHARACTER, {1, 0}},
    {contracts::QolPortId::QOL_PORT_CHARACTER_HTTP, {1, 0}},
};

const contracts::CapabilityRequirement kRequires[] = {
    {contracts::kHttpRegistryCapabilityId, {contracts::kHttpRegistryVersion.major,
                                             contracts::kHttpRegistryVersion.minor}},
    {contracts::kHttpDispatchCapabilityId, {contracts::kHttpDispatchVersion.major,
                                             contracts::kHttpDispatchVersion.minor}},
};

QolResult not_ready() {
    return contracts::error(kQolErrNotReady, "character facts not ready", 25);
}

QolResult type(const void* character, int32_t* out_type) {
    if (g_port == nullptr || g_port->read.type == nullptr) return not_ready();
    return g_port->read.type(character, out_type);
}

QolResult is_active_player_group(const void* character, int32_t* out_active) {
    if (g_port == nullptr || g_port->read.is_active_player_group == nullptr) {
        return not_ready();
    }
    return g_port->read.is_active_player_group(character, out_active);
}

constexpr contracts::CharacterFactsApi kCharacterFactsApi{
    &type,
    &is_active_player_group,
};

const contracts::CapabilityProvisionRef kProvides[] = {
    {contracts::kCharacterFactsCapabilityId, contracts::kCharacterFactsVersion, &kCharacterFactsApi},
};

void log_message(const contracts::QolHostSdk* sdk, const char* message) {
    if (sdk == nullptr || sdk->log.write == nullptr || message == nullptr) return;
    uint32_t length = 0;
    while (message[length] != '\0') ++length;
    (void)sdk->log.write(1, 1, kPluginId, message, length);
}

bool is_error_body(const char* body, uint32_t size) {
    return size >= 9 && std::strncmp(body, "{\"error\":", 9) == 0;
}

bool fetch_json(int32_t kind, char* out, uint32_t capacity, uint32_t* out_size) {
    if (g_http_port == nullptr || g_http_port->read.body_json == nullptr) return false;
    *out_size = 0;
    return contracts::succeeded(g_http_port->read.body_json(kind, out, capacity, out_size)) &&
           *out_size > 0;
}

void update_snapshot() {
    if (g_http_port == nullptr || g_http_port->read.body_json == nullptr) return;
    CharacterSnapshot next{};
    const auto kPlayer = static_cast<int32_t>(contracts::QolCharacterHttpJsonKind::kPlayer);
    const auto kParty = static_cast<int32_t>(contracts::QolCharacterHttpJsonKind::kParty);
    const auto kSkills = static_cast<int32_t>(contracts::QolCharacterHttpJsonKind::kSkills);
    const auto kMercenaries =
        static_cast<int32_t>(contracts::QolCharacterHttpJsonKind::kMercenaries);
    if (!fetch_json(kPlayer, next.player, kPlayerCapacity, &next.player_size)) return;
    if (!fetch_json(kParty, next.party, kPartyCapacity, &next.party_size)) return;
    if (!fetch_json(kSkills, next.skills, kSkillsCapacity, &next.skills_size)) return;
    if (!fetch_json(kMercenaries, next.mercenaries, kMercenariesCapacity,
                    &next.mercenaries_size)) {
        return;
    }
    // 非 world 态四类原生体均为 {"error":"not in game"}；任意一类报错即整轮不可用。
    if (is_error_body(next.player, next.player_size) ||
        is_error_body(next.party, next.party_size) ||
        is_error_body(next.skills, next.skills_size) ||
        is_error_body(next.mercenaries, next.mercenaries_size)) {
        next.state = SnapshotState::kNotInGame;
        g_snapshot.publish(next);
        return;
    }
    next.state = SnapshotState::kReady;
    g_snapshot.publish(next);
}

int32_t frame_tick(int64_t, void*) {
    update_snapshot();
    return 1;
}

QolResult response_error(contracts::HttpRes* response, uint16_t status, const char* message) {
    const std::string body = std::string("{\"ok\":false,\"error\":\"") + message + "\"}";
    if (response == nullptr || response->body == nullptr || response->body_capacity < body.size()) {
        return contracts::error(kQolErrInternal, "http response buffer too small", 31);
    }
    std::memcpy(response->body, body.data(), body.size());
    response->body_size = static_cast<uint32_t>(body.size());
    response->status = status;
    return contracts::error(status == 503 ? kQolErrNotReady : kQolErrInvalidArgument, message,
                            static_cast<uint32_t>(std::strlen(message)));
}

QolResult response_body(contracts::HttpRes* response, const char* body, uint32_t size) {
    if (response == nullptr || response->body == nullptr || body == nullptr ||
        response->body_capacity < size) {
        return response_error(response, 500, "http response buffer too small");
    }
    std::memcpy(response->body, body, size);
    response->body_size = size;
    response->status = 200;
    return contracts::ok();
}

QolResult not_found(contracts::HttpRes* response) {
    return response_error(response, 404, "not found");
}

bool parse_path_int(const contracts::HttpReq& request, const char* name, int32_t* out) {
    if (request.path_params == nullptr || name == nullptr || out == nullptr) return false;
    for (uint32_t i = 0; i < request.path_param_count; ++i) {
        const contracts::HttpPathParam& param = request.path_params[i];
        if (param.name == nullptr || param.name_len != std::strlen(name) ||
            std::strncmp(param.name, name, param.name_len) != 0) {
            continue;
        }
        if (param.value == nullptr || param.value_len == 0) return false;
        const std::string value(param.value, param.value_len);
        char* end = nullptr;
        const long parsed = std::strtol(value.c_str(), &end, 10);
        if (end == nullptr || *end != '\0') return false;
        *out = static_cast<int32_t>(parsed);
        return true;
    }
    return false;
}

// 追加成员整数字段：缺失或负值跳过（迁移前 `optInt(key,-1).takeIf{it>=0}`）。
void append_int_field(std::string& out, bool* first, const char* key, const char* json,
                      uint32_t size, const char* member) {
    long long value = 0;
    if (!json_int_member(json, size, member, &value) || value < 0) return;
    if (!*first) out += ",";
    *first = false;
    out += "\"";
    out += key;
    out += "\":" + std::to_string(value);
}

QolResult query_handler(const contracts::HttpReq* request, contracts::HttpRes* response, void* ctx) {
    if (request == nullptr || response == nullptr || ctx == nullptr) {
        return response_error(response, 400, "bad request");
    }
    CharacterSnapshot snapshot{};
    if (!g_snapshot.read(snapshot) || snapshot.state == SnapshotState::kUnavailable) {
        return response_error(response, 503, "snapshot not ready");
    }
    if (snapshot.state == SnapshotState::kNotInGame) {
        return response_body(response, kNotInGameBody,
                             static_cast<uint32_t>(sizeof(kNotInGameBody) - 1));
    }

    const char* party = snapshot.party;
    const uint32_t party_size = snapshot.party_size;
    const char* skills = snapshot.skills;
    const uint32_t skills_size = snapshot.skills_size;
    const char* mercenaries = snapshot.mercenaries;
    const uint32_t mercenaries_size = snapshot.mercenaries_size;
    const char* player = snapshot.player;
    const uint32_t player_size = snapshot.player_size;

    const Query query = *static_cast<const Query*>(ctx);
    switch (query) {
        case Query::kMercenary:
            return response_body(response, mercenaries, mercenaries_size);
        case Query::kMercenaryList: {
            int32_t count = 0;
            if (!json_array_size(mercenaries, mercenaries_size, &count)) {
                count = 0;
            }
            std::string body = "{\"slots\":[";
            bool first = true;
            for (int32_t i = 0; i < count; ++i) {
                JsonSlice element{};
                if (!json_element(mercenaries, mercenaries_size, i, &element)) break;
                long long slot = -1;
                if (!json_int_member(element.data, element.size, "slot", &slot) || slot < 0) {
                    continue;
                }
                if (!first) body += ",";
                first = false;
                body += std::to_string(slot);
            }
            body += "]}";
            return response_body(response, body.data(), static_cast<uint32_t>(body.size()));
        }
        case Query::kMercenarySlot: {
            int32_t requested = 0;
            if (!parse_path_int(*request, "slot", &requested)) {
                return response_error(response, 400, "bad request");
            }
            int32_t count = 0;
            if (!json_array_size(mercenaries, mercenaries_size, &count)) {
                return not_found(response);
            }
            for (int32_t i = 0; i < count; ++i) {
                JsonSlice element{};
                if (!json_element(mercenaries, mercenaries_size, i, &element)) break;
                long long slot = -1;
                if (!json_int_member(element.data, element.size, "slot", &slot)) continue;
                if (slot == requested) {
                    return response_body(response, element.data, element.size);
                }
            }
            return not_found(response);
        }
        case Query::kParty:
            return response_body(response, party, party_size);
        case Query::kPartyCount: {
            long long count = -1;
            if (!json_int_member(player, player_size, "party_count", &count)) count = -1;
            const std::string body = "{\"count\":" + std::to_string(count) + "}";
            return response_body(response, body.data(), static_cast<uint32_t>(body.size()));
        }
        case Query::kLeader: {
            long long leader_slot = 0;
            if (!json_int_member(player, player_size, "leader_slot", &leader_slot)) leader_slot = 0;
            JsonSlice member{};
            if (leader_slot < 0 ||
                !json_element(party, party_size, static_cast<int32_t>(leader_slot), &member) ||
                json_slice_is_null(member)) {
                return not_found(response);
            }
            return response_body(response, member.data, member.size);
        }
        case Query::kPartyMember: {
            int32_t slot = 0;
            if (!parse_path_int(*request, "slot", &slot)) {
                return response_error(response, 400, "bad request");
            }
            JsonSlice member{};
            if (!json_element(party, party_size, slot, &member) || json_slice_is_null(member)) {
                return not_found(response);
            }
            return response_body(response, member.data, member.size);
        }
        case Query::kPartyMemberId: {
            int32_t slot = 0;
            if (!parse_path_int(*request, "slot", &slot)) {
                return response_error(response, 400, "bad request");
            }
            JsonSlice member{};
            if (!json_element(party, party_size, slot, &member) || json_slice_is_null(member)) {
                return not_found(response);
            }
            long long class_idx = -1;
            if (!json_int_member(member.data, member.size, "class_idx", &class_idx) ||
                class_idx < 0) {
                return not_found(response);
            }
            std::string body = "{\"id\":" + std::to_string(class_idx);
            JsonSlice class_name{};
            if (json_member(member.data, member.size, "class_name", &class_name) &&
                !json_slice_is_null(class_name)) {
                body += ",\"id_name\":" + std::string(class_name.data, class_name.size);
            }
            body += "}";
            return response_body(response, body.data(), static_cast<uint32_t>(body.size()));
        }
        case Query::kPartyMemberName: {
            int32_t slot = 0;
            if (!parse_path_int(*request, "slot", &slot)) {
                return response_error(response, 400, "bad request");
            }
            JsonSlice member{};
            if (!json_element(party, party_size, slot, &member) || json_slice_is_null(member)) {
                return not_found(response);
            }
            JsonSlice name{};
            if (!json_member(member.data, member.size, "name", &name)) return not_found(response);
            const std::string body = "{\"name\":" + std::string(name.data, name.size) + "}";
            return response_body(response, body.data(), static_cast<uint32_t>(body.size()));
        }
        case Query::kPartyMemberLevel: {
            int32_t slot = 0;
            if (!parse_path_int(*request, "slot", &slot)) {
                return response_error(response, 400, "bad request");
            }
            JsonSlice member{};
            if (!json_element(party, party_size, slot, &member) || json_slice_is_null(member)) {
                return not_found(response);
            }
            long long level = 0;
            if (!json_int_member(member.data, member.size, "level", &level)) {
                return not_found(response);
            }
            const std::string body = "{\"level\":" + std::to_string(level) + "}";
            return response_body(response, body.data(), static_cast<uint32_t>(body.size()));
        }
        case Query::kPartyMemberStatus: {
            int32_t slot = 0;
            if (!parse_path_int(*request, "slot", &slot)) {
                return response_error(response, 400, "bad request");
            }
            JsonSlice member{};
            if (!json_element(party, party_size, slot, &member) || json_slice_is_null(member)) {
                return not_found(response);
            }
            std::string status = "{";
            bool first = true;
            append_int_field(status, &first, "hp", member.data, member.size, "hp");
            append_int_field(status, &first, "max_hp", member.data, member.size, "max_hp");
            append_int_field(status, &first, "mp", member.data, member.size, "mp");
            append_int_field(status, &first, "max_mp", member.data, member.size, "max_mp");
            append_int_field(status, &first, "exp", member.data, member.size, "exp");
            append_int_field(status, &first, "exp_next", member.data, member.size, "exp_next");
            append_int_field(status, &first, "attribute_points", member.data, member.size,
                             "status_point");
            JsonSlice skills_element{};
            if (json_element(skills, skills_size, slot, &skills_element) &&
                !json_slice_is_null(skills_element)) {
                append_int_field(status, &first, "skill_points", skills_element.data,
                                 skills_element.size, "skill_points");
            }
            status += "}";
            const std::string body = "{\"status\":" + status + "}";
            return response_body(response, body.data(), static_cast<uint32_t>(body.size()));
        }
        case Query::kPartyMemberStats: {
            int32_t slot = 0;
            if (!parse_path_int(*request, "slot", &slot)) {
                return response_error(response, 400, "bad request");
            }
            JsonSlice member{};
            if (!json_element(party, party_size, slot, &member) || json_slice_is_null(member)) {
                const std::string body = "{\"stats\":null}";
                return response_body(response, body.data(), static_cast<uint32_t>(body.size()));
            }
            JsonSlice stats{};
            if (!json_member(member.data, member.size, "stats", &stats) ||
                json_slice_is_null(stats)) {
                const std::string body = "{\"stats\":null}";
                return response_body(response, body.data(), static_cast<uint32_t>(body.size()));
            }
            const std::string body = "{\"stats\":" + std::string(stats.data, stats.size) + "}";
            return response_body(response, body.data(), static_cast<uint32_t>(body.size()));
        }
        case Query::kPartyMemberEquipment: {
            int32_t slot = 0;
            if (!parse_path_int(*request, "slot", &slot)) {
                return response_error(response, 400, "bad request");
            }
            JsonSlice member{};
            if (!json_element(party, party_size, slot, &member) || json_slice_is_null(member)) {
                return not_found(response);
            }
            JsonSlice equipment{};
            if (!json_member(member.data, member.size, "equipment", &equipment) ||
                json_slice_is_null(equipment)) {
                return not_found(response);
            }
            const std::string body =
                "{\"equipment\":" + std::string(equipment.data, equipment.size) + "}";
            return response_body(response, body.data(), static_cast<uint32_t>(body.size()));
        }
        case Query::kPartyMemberEquip: {
            int32_t slot = 0;
            int32_t equip_slot = 0;
            if (!parse_path_int(*request, "slot", &slot) ||
                !parse_path_int(*request, "equip_slot", &equip_slot)) {
                return response_error(response, 400, "bad request");
            }
            JsonSlice member{};
            if (!json_element(party, party_size, slot, &member) || json_slice_is_null(member)) {
                return not_found(response);
            }
            JsonSlice equipment{};
            if (!json_member(member.data, member.size, "equipment", &equipment) ||
                json_slice_is_null(equipment)) {
                return not_found(response);
            }
            JsonSlice item{};
            if (!json_element(equipment.data, equipment.size, equip_slot, &item) ||
                json_slice_is_null(item)) {
                return not_found(response);
            }
            return response_body(response, item.data, item.size);
        }
        case Query::kPartyMemberSkills: {
            int32_t slot = 0;
            if (!parse_path_int(*request, "slot", &slot)) {
                return response_error(response, 400, "bad request");
            }
            JsonSlice element{};
            if (!json_element(skills, skills_size, slot, &element) ||
                json_slice_is_null(element)) {
                return not_found(response);
            }
            return response_body(response, element.data, element.size);
        }
    }
    return response_error(response, 500, "unknown character query");
}

const contracts::HttpQueryHandlerDescriptor kHttpQueries[] = {
    {kRoutes[0].method, kRoutes[0].path, kPluginId, &query_handler,
     const_cast<Query*>(&kQueries[0])},
    {kRoutes[1].method, kRoutes[1].path, kPluginId, &query_handler,
     const_cast<Query*>(&kQueries[1])},
    {kRoutes[2].method, kRoutes[2].path, kPluginId, &query_handler,
     const_cast<Query*>(&kQueries[2])},
    {kRoutes[3].method, kRoutes[3].path, kPluginId, &query_handler,
     const_cast<Query*>(&kQueries[3])},
    {kRoutes[4].method, kRoutes[4].path, kPluginId, &query_handler,
     const_cast<Query*>(&kQueries[4])},
    {kRoutes[5].method, kRoutes[5].path, kPluginId, &query_handler,
     const_cast<Query*>(&kQueries[5])},
    {kRoutes[6].method, kRoutes[6].path, kPluginId, &query_handler,
     const_cast<Query*>(&kQueries[6])},
    {kRoutes[7].method, kRoutes[7].path, kPluginId, &query_handler,
     const_cast<Query*>(&kQueries[7])},
    {kRoutes[8].method, kRoutes[8].path, kPluginId, &query_handler,
     const_cast<Query*>(&kQueries[8])},
    {kRoutes[9].method, kRoutes[9].path, kPluginId, &query_handler,
     const_cast<Query*>(&kQueries[9])},
    {kRoutes[10].method, kRoutes[10].path, kPluginId, &query_handler,
     const_cast<Query*>(&kQueries[10])},
    {kRoutes[11].method, kRoutes[11].path, kPluginId, &query_handler,
     const_cast<Query*>(&kQueries[11])},
    {kRoutes[12].method, kRoutes[12].path, kPluginId, &query_handler,
     const_cast<Query*>(&kQueries[12])},
    {kRoutes[13].method, kRoutes[13].path, kPluginId, &query_handler,
     const_cast<Query*>(&kQueries[13])},
    {kRoutes[14].method, kRoutes[14].path, kPluginId, &query_handler,
     const_cast<Query*>(&kQueries[14])},
};

void stop_frame_task(const contracts::QolHostSdk* sdk) {
    if (g_frame_task != 0 && sdk != nullptr && sdk->scheduler.remove != nullptr) {
        (void)sdk->scheduler.remove(g_frame_task);
    }
    g_frame_task = 0;
}

int plugin_install(contracts::PluginContext& ctx) {
    g_sdk = ctx.sdk;
    g_port = nullptr;
    g_http_port = nullptr;
    g_frame_task = 0;
    CharacterSnapshot empty_snapshot{};
    g_snapshot.publish(empty_snapshot);
    if (ctx.struct_size < contracts::PLUGIN_CONTEXT_PORTS_STRUCT_SIZE ||
        ctx.sdk_major != contracts::kHostSdkMajor || g_sdk == nullptr || ctx.ports == nullptr ||
        ctx.port_count != 2 || ctx.ports[0] == nullptr || ctx.ports[1] == nullptr) {
        g_sdk = nullptr;
        return 1;
    }
    g_port = static_cast<const contracts::QolCharacterPort*>(ctx.ports[0]);
    g_http_port = static_cast<const contracts::QolCharacterHttpPort*>(ctx.ports[1]);
    if (!contracts::satisfies(g_port->abi_version, {1, 0}) || g_port->read.type == nullptr ||
        g_port->read.is_active_player_group == nullptr ||
        !contracts::satisfies(g_http_port->abi_version, {1, 0}) ||
        g_http_port->read.body_json == nullptr) {
        g_port = nullptr;
        g_http_port = nullptr;
        g_sdk = nullptr;
        return 1;
    }
    log_message(g_sdk, "[core.character] provider install");
    return 0;
}

void plugin_deactivate(contracts::PluginContext& ctx) {
    stop_frame_task(ctx.sdk);
    log_message(ctx.sdk, "[core.character] provider deactivate");
    g_port = nullptr;
    g_http_port = nullptr;
    g_sdk = nullptr;
}

void plugin_on_enabled(contracts::PluginContext& ctx) {
    CharacterSnapshot empty_snapshot{};
    g_snapshot.publish(empty_snapshot);
    if (g_frame_task == 0 && g_sdk != nullptr && g_sdk->scheduler.add != nullptr) {
        uint64_t task = 0;
        if (contracts::succeeded(
                g_sdk->scheduler.add(1, &frame_tick, nullptr, kSnapshotIntervalFrames, 0, &task))) {
            g_frame_task = task;
        } else {
            log_message(ctx.sdk, "[core.character] snapshot scheduler unavailable");
        }
    }
    log_message(ctx.sdk, "[core.character] provider enabled: capability + http queries");
}

void plugin_on_disabled(contracts::PluginContext& ctx) {
    stop_frame_task(ctx.sdk);
    CharacterSnapshot empty_snapshot{};
    g_snapshot.publish(empty_snapshot);
    log_message(ctx.sdk, "[core.character] provider disabled: capability + http queries withdrawn");
}

void plugin_fill_status(contracts::PluginStatusOut& out) {
    out.enabled = g_port != nullptr;
    out.error = nullptr;
}

const contracts::PluginDescriptor kDescriptor = {
    kPluginId,
    kPluginVersion,
    2,
    kRequires,
    0,
    nullptr,
    1,
    kProvides,
    0,
    nullptr,
    0,
    nullptr,
    &plugin_install,
    &plugin_deactivate,
    &plugin_on_enabled,
    &plugin_on_disabled,
    &plugin_fill_status,
    nullptr,
    kPorts,
    2,
    kHttpQueries,
    static_cast<uint32_t>(sizeof(kHttpQueries) / sizeof(kHttpQueries[0])),
};

}  // namespace

const contracts::PluginDescriptor& plugin_descriptor() {
    return kDescriptor;
}

}  // namespace qol::features::core_character
