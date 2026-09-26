#include "plugin.h"

#include <cstdint>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <string>
#include <vector>

#include "contracts/character_facts.h"
#include "contracts/character_state.h"
#include "contracts/game_ports/character_http_port.h"
#include "contracts/game_ports/character_state_port.h"
#include "contracts/commands.h"
#include "contracts/game_ports/character_port.h"
#include "contracts/game_ports/character_write_ops_port.h"
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
// 写面 typed 低层原语端口（仅 core.character 可注入）：owner 侧按 kind 分派后调用。
const contracts::QolCharacterWriteOpsPort* g_character_write_ops = nullptr;
const contracts::QolCharacterStatePort* g_character_state_port = nullptr;
uint64_t g_frame_task = 0;
Snapshot<CharacterSnapshot> g_snapshot;

const contracts::QolPortRequirement kPorts[] = {
    {contracts::QolPortId::QOL_PORT_CHARACTER, {1, 0}},
    {contracts::QolPortId::QOL_PORT_CHARACTER_HTTP, {1, 0}},
    {contracts::QolPortId::QOL_PORT_CHARACTER_WRITE_OPS,
     {contracts::kQolPortCharacterWriteOpsVersion.major,
      contracts::kQolPortCharacterWriteOpsVersion.minor}},
    {contracts::QolPortId::QOL_PORT_CHARACTER_STATE,
     {contracts::kQolPortCharacterStateVersion.major,
      contracts::kQolPortCharacterStateVersion.minor}},
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

QolResult write_character_state_json(contracts::QolCharacterStateKind kind, char* out,
                                     uint32_t capacity, uint32_t* out_size);
const contracts::CharacterStateApi kCharacterStateApi{&write_character_state_json};

const contracts::CapabilityProvisionRef kProvides[] = {
    {contracts::kCharacterFactsCapabilityId, contracts::kCharacterFactsVersion, &kCharacterFactsApi},
    {contracts::kCharacterStateCapabilityId, contracts::kCharacterStateVersion, &kCharacterStateApi},
};

void append_json_string(std::string& out, const char* value, uint32_t size) {
    for (uint32_t i = 0; i < size; ++i) {
        const unsigned char ch = static_cast<unsigned char>(value[i]);
        switch (ch) {
            case '\\': out += "\\\\"; break;
            case '"': out += "\\\""; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (ch < 0x20) {
                    char escaped[7]{};
                    std::snprintf(escaped, sizeof(escaped), "\\u%04x", ch);
                    out += escaped;
                } else {
                    out += static_cast<char>(ch);
                }
                break;
        }
    }
}

void append_character_state_item(std::string& out, uint32_t slot,
                                 const contracts::QolCharacterStateItem& item) {
    out += "{\"slot\":" + std::to_string(slot);
    out += ",\"type_flags\":" + std::to_string(item.type_flags);
    out += ",\"raw_rarity\":" + std::to_string(item.raw_rarity);
    if ((item.fields & contracts::kQolCharacterStateItemHasCategory) != 0) {
        out += ",\"category\":" + std::to_string(item.category);
    }
    if ((item.fields & contracts::kQolCharacterStateItemHasCount) != 0) {
        out += ",\"count\":" + std::to_string(item.count);
    }
    out += item.equip != 0 ? ",\"equip\":true" : ",\"equip\":false";
    if ((item.fields & contracts::kQolCharacterStateItemHasRarity) != 0) {
        out += ",\"rarity\":" + std::to_string(item.rarity);
    }
    if ((item.fields & contracts::kQolCharacterStateItemHasDamage) != 0) {
        out += ",\"damage\":" + std::to_string(item.damage);
    }
    if ((item.fields & contracts::kQolCharacterStateItemHasDefense) != 0) {
        out += ",\"defense\":" + std::to_string(item.defense);
    }
    out += ",\"magic_rate\":" + std::to_string(item.magic_rate);
    if ((item.fields & contracts::kQolCharacterStateItemHasNeedLevel) != 0) {
        out += ",\"need_level\":" + std::to_string(item.need_level);
    }
    out += ",\"socket\":" + std::to_string(item.socket);
    out += ",\"socket_filled\":" + std::to_string(item.socket_filled);
    out += ",\"socket_total\":" + std::to_string(item.socket_total);
    out += ",\"enchant\":" + std::to_string(item.enchant);
    out += item.chaos != 0 ? ",\"chaos\":true" : ",\"chaos\":false";
    out += ",\"enchant_id\":" + std::to_string(item.enchant_id);
    out += ",\"enchant_level\":" + std::to_string(item.enchant_level);
    out += ",\"chaos_level\":" + std::to_string(item.chaos_level);
    out += ",\"chaos_rate\":" + std::to_string(item.chaos_rate);
    out += ",\"options\":[";
    for (uint32_t i = 0; i < item.option_count; ++i) {
        if (i != 0) out.push_back(',');
        out += std::to_string(item.options[i].value);
    }
    out += "],\"option_ids\":[";
    for (uint32_t i = 0; i < item.option_count; ++i) {
        if (i != 0) out.push_back(',');
        out += std::to_string(item.options[i].id);
    }
    out += "],\"option_types\":[";
    for (uint32_t i = 0; i < item.option_count; ++i) {
        if (i != 0) out.push_back(',');
        out += std::to_string(item.options[i].type);
    }
    out += "]}";
}

void append_character_state_member(std::string& out,
                                   const contracts::QolCharacterStateMember& member) {
    out += "{\"type\":" + std::to_string(member.type);
    out += ",\"class_idx\":" + std::to_string(member.class_idx);
    out += ",\"name_id\":" + std::to_string(member.name_id);
    out += ",\"level\":" + std::to_string(member.level);
    out += ",\"hp\":" + std::to_string(member.hp);
    out += ",\"mp\":" + std::to_string(member.mp);
    out += ",\"max_hp\":" + std::to_string(member.max_hp);
    out += ",\"max_mp\":" + std::to_string(member.max_mp);
    out += ",\"exp\":" + std::to_string(member.exp);
    out += ",\"exp_next\":" + std::to_string(member.exp_next);
    out += ",\"stats\":{";
    for (uint32_t i = 0; i < contracts::kQolCharacterStateStatCount; ++i) {
        if (i != 0) out.push_back(',');
        out += "\"" + std::to_string(i) + "\":" + std::to_string(member.stats[i]);
    }
    out += "},\"main_stats\":[";
    for (uint32_t i = 0; i < contracts::kQolCharacterStateMainStatCount; ++i) {
        if (i != 0) out.push_back(',');
        out += std::to_string(member.main_stats[i]);
    }
    out.push_back(']');
    if ((member.fields & contracts::kQolCharacterStateHasBaseStats) != 0) {
        out += ",\"base_stats\":[";
        for (uint32_t i = 0; i < contracts::kQolCharacterStateMainStatCount; ++i) {
            if (i != 0) out.push_back(',');
            out += std::to_string(member.base_stats[i]);
        }
        out.push_back(']');
    }
    if ((member.fields & contracts::kQolCharacterStateHasStatusPoint) != 0) {
        out += ",\"status_point\":" + std::to_string(member.status_point);
    }
    out += ",\"equipment\":[";
    for (uint32_t slot = 0; slot < contracts::kQolCharacterStateEquipSlots; ++slot) {
        if (slot != 0) out.push_back(',');
        if (member.equipment_present[slot] == 0) {
            out += "null";
        } else {
            append_character_state_item(out, slot, member.equipment[slot]);
        }
    }
    out.push_back(']');
    if ((member.fields & contracts::kQolCharacterStateHasName) != 0) {
        out += ",\"name\":\"";
        append_json_string(out, member.name, member.name_size);
        out.push_back('"');
    }
    out.push_back('}');
}

std::string serialize_character_state_party(const contracts::QolCharacterStateParty& party) {
    std::string out = "[";
    for (uint32_t i = 0; i < contracts::kQolCharacterStatePartySize; ++i) {
        if (i != 0) out.push_back(',');
        if (party.member_present[i] == 0) {
            out += "null";
        } else {
            append_character_state_member(out, party.members[i]);
        }
    }
    out.push_back(']');
    return out;
}

std::string serialize_character_state_player(const contracts::QolCharacterStatePlayer& player) {
    return "{\"money\":" + std::to_string(player.money) +
           ",\"map_id\":" + std::to_string(player.map_id) +
           ",\"x\":" + std::to_string(player.x) +
           ",\"y\":" + std::to_string(player.y) +
           ",\"active_quest\":" + std::to_string(player.active_quest) +
           ",\"leader_slot\":" + std::to_string(player.leader_slot) +
           ",\"party_count\":" + std::to_string(player.party_count) + "}";
}

std::string serialize_character_state_skills(const contracts::QolCharacterStateSkills& skills) {
    std::string out = "[";
    for (uint32_t role = 0; role < contracts::kQolCharacterStatePartySize; ++role) {
        if (role != 0) out.push_back(',');
        if (skills.member_present[role] == 0) {
            out += "null";
            continue;
        }
        const auto& member = skills.members[role];
        out += "{\"role\":" + std::to_string(role) + ",\"skills\":[";
        for (uint32_t i = 0; i < member.skill_count; ++i) {
            if (i != 0) out.push_back(',');
            const auto& skill = member.skills[i];
            out += "{\"action_id\":" + std::to_string(skill.action_id) +
                   ",\"level\":" + std::to_string(skill.level);
            if (skill.has_max_level != 0) {
                out += ",\"max_level\":" + std::to_string(skill.max_level);
            }
            out.push_back('}');
        }
        out += "],\"unlock_bitmap\":" + std::to_string(member.unlock_bitmap) +
               ",\"active_skill_id\":" + std::to_string(member.active_skill_id) +
               ",\"skill_points\":" + std::to_string(member.skill_points) + "}";
    }
    out.push_back(']');
    return out;
}

QolResult write_character_state_json(contracts::QolCharacterStateKind kind, char* out,
                                     uint32_t capacity, uint32_t* out_size) {
    if (out_size != nullptr) *out_size = 0;
    if (out == nullptr || out_size == nullptr || g_character_state_port == nullptr) {
        return contracts::error(kQolErrNotReady, "character state unavailable", 27);
    }
    std::string body;
    QolResult read_result = contracts::ok();
    switch (kind) {
        case contracts::QolCharacterStateKind::kParty: {
            contracts::QolCharacterStateParty state{};
            if (g_character_state_port->read.party == nullptr) return not_ready();
            read_result = g_character_state_port->read.party(&state);
            if (contracts::succeeded(read_result)) body = serialize_character_state_party(state);
            break;
        }
        case contracts::QolCharacterStateKind::kPlayer: {
            contracts::QolCharacterStatePlayer state{};
            if (g_character_state_port->read.player == nullptr) return not_ready();
            read_result = g_character_state_port->read.player(&state);
            if (contracts::succeeded(read_result)) body = serialize_character_state_player(state);
            break;
        }
        case contracts::QolCharacterStateKind::kSkills: {
            contracts::QolCharacterStateSkills state{};
            if (g_character_state_port->read.skills == nullptr) return not_ready();
            read_result = g_character_state_port->read.skills(&state);
            if (contracts::succeeded(read_result)) body = serialize_character_state_skills(state);
            break;
        }
        default:
            return contracts::error(kQolErrInvalidArgument, "invalid character state kind", 28);
    }
    if (!contracts::succeeded(read_result)) {
        if (read_result.message != nullptr && read_result.message_len == 11 &&
            std::memcmp(read_result.message, "not in game", 11) == 0) {
            body = "{\"error\":\"not in game\"}";
        } else {
            return read_result;
        }
    }
    if (body.size() > capacity) {
        return contracts::error(kQolErrBlocked, "character state buffer too small", 32);
    }
    std::memcpy(out, body.data(), body.size());
    *out_size = static_cast<uint32_t>(body.size());
    return contracts::ok();
}

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

// ---- 写面（20 条声明式 POST；阶段 5 写面迁移）----
// HTTP 线程只解析与投递票据；游戏写在主线程结果处理器内执行（owner 命令 + 票据契约）。
constexpr uint32_t kCharWriteHandlerId = 0x43480001;

// 私有载荷：单点 kind 用 i0..i2/i64；批量 kind（add_stat/set_attr）用 point_count + 定长数组
// （端口参数集装不下点集；4*5+8+4+64 = 96B，≤ 128B typed payload 上限）。
struct CharWritePayload {
    int32_t kind = 0;
    int32_t i0 = -1;
    int32_t i1 = -1;
    int32_t i2 = -1;
    int64_t i64 = 0;
    int32_t point_count = 0;
    int32_t point_idx[8] = {};
    int32_t point_amt[8] = {};
};

struct CharJsonValue {
    enum class Type : uint8_t { kNull, kBool, kNumber, kString, kObject, kArray };
    Type type = Type::kNull;
    bool boolean = false;
    std::string scalar;
    std::vector<std::string> keys;
    std::vector<CharJsonValue> values;
};

void char_json_ws(const std::string& text, size_t* pos) {
    while (*pos < text.size() && (text[*pos] == ' ' || text[*pos] == '\t' ||
                                  text[*pos] == '\n' || text[*pos] == '\r')) ++*pos;
}

bool char_json_string(const std::string& text, size_t* pos, std::string* out) {
    if (*pos >= text.size() || text[*pos] != '"') return false;
    ++*pos;
    while (*pos < text.size()) {
        const char c = text[(*pos)++];
        if (c == '"') return true;
        if (c == '\\') {
            if (*pos >= text.size()) return false;
            const char escaped = text[(*pos)++];
            if (escaped == 'u') {
                if (text.size() - *pos < 4) return false;
                for (int i = 0; i < 4; ++i) {
                    const char h = text[(*pos)++];
                    if (!((h >= '0' && h <= '9') || (h >= 'a' && h <= 'f') ||
                          (h >= 'A' && h <= 'F'))) return false;
                }
                if (out != nullptr) out->push_back('?');
            } else {
                if (escaped != '"' && escaped != '\\' && escaped != '/' && escaped != 'b' &&
                    escaped != 'f' && escaped != 'n' && escaped != 'r' && escaped != 't') return false;
                if (out != nullptr) out->push_back(escaped);
            }
        } else {
            if (static_cast<unsigned char>(c) < 0x20) return false;
            if (out != nullptr) out->push_back(c);
        }
    }
    return false;
}

bool char_json_value(const std::string& text, size_t* pos, CharJsonValue* out, uint32_t depth = 0) {
    if (depth > 64) return false;
    char_json_ws(text, pos);
    if (*pos >= text.size()) return false;
    const char c = text[*pos];
    if (c == '{' || c == '[') {
        const bool object = c == '{';
        out->type = object ? CharJsonValue::Type::kObject : CharJsonValue::Type::kArray;
        ++*pos;
        char_json_ws(text, pos);
        const char end = object ? '}' : ']';
        if (*pos < text.size() && text[*pos] == end) { ++*pos; return true; }
        while (*pos < text.size()) {
            std::string key;
            if (object && !char_json_string(text, pos, &key)) return false;
            char_json_ws(text, pos);
            if (object) {
                if (*pos >= text.size() || text[(*pos)++] != ':') return false;
                char_json_ws(text, pos);
            }
            CharJsonValue value{};
            if (!char_json_value(text, pos, &value, depth + 1)) return false;
            if (object) {
                size_t existing = 0;
                while (existing < out->keys.size() && out->keys[existing] != key) ++existing;
                if (existing == out->keys.size()) {
                    out->keys.push_back(std::move(key));
                    out->values.push_back(std::move(value));
                } else {
                    out->values[existing] = std::move(value);
                }
            } else {
                out->values.push_back(std::move(value));
            }
            char_json_ws(text, pos);
            if (*pos < text.size() && text[*pos] == ',') { ++*pos; char_json_ws(text, pos); continue; }
            if (*pos < text.size() && text[*pos] == end) { ++*pos; return true; }
            return false;
        }
        return false;
    }
    if (c == '"') {
        out->type = CharJsonValue::Type::kString;
        return char_json_string(text, pos, &out->scalar);
    }
    const size_t begin = *pos;
    while (*pos < text.size() && text[*pos] != ',' && text[*pos] != '}' && text[*pos] != ']' &&
           text[*pos] != ' ' && text[*pos] != '\t' && text[*pos] != '\n' && text[*pos] != '\r') ++*pos;
    if (*pos == begin) return false;
    out->scalar = text.substr(begin, *pos - begin);
    if (out->scalar == "true" || out->scalar == "false") {
        out->type = CharJsonValue::Type::kBool;
        out->boolean = out->scalar == "true";
        return true;
    }
    if (out->scalar == "null") { out->type = CharJsonValue::Type::kNull; return true; }
    size_t number = 0;
    if (out->scalar[number] == '-') ++number;
    if (number >= out->scalar.size()) return false;
    if (out->scalar[number] == '0') {
        ++number;
        if (number < out->scalar.size() && out->scalar[number] >= '0' && out->scalar[number] <= '9') return false;
    } else {
        if (out->scalar[number] < '1' || out->scalar[number] > '9') return false;
        while (number < out->scalar.size() && out->scalar[number] >= '0' && out->scalar[number] <= '9') ++number;
    }
    if (number < out->scalar.size() && out->scalar[number] == '.') {
        ++number;
        const size_t fraction = number;
        while (number < out->scalar.size() && out->scalar[number] >= '0' && out->scalar[number] <= '9') ++number;
        if (fraction == number) return false;
    }
    if (number < out->scalar.size() && (out->scalar[number] == 'e' || out->scalar[number] == 'E')) {
        ++number;
        if (number < out->scalar.size() && (out->scalar[number] == '+' || out->scalar[number] == '-')) ++number;
        const size_t exponent = number;
        while (number < out->scalar.size() && out->scalar[number] >= '0' && out->scalar[number] <= '9') ++number;
        if (exponent == number) return false;
    }
    if (number != out->scalar.size()) return false;
    out->type = CharJsonValue::Type::kNumber;
    return true;
}

bool char_parse_json(const std::string& text, CharJsonValue* out) {
    size_t pos = 0;
    if (out == nullptr || !char_json_value(text, &pos, out)) return false;
    char_json_ws(text, &pos);
    return pos == text.size();
}

const CharJsonValue* char_member(const CharJsonValue& object, const char* key) {
    if (object.type != CharJsonValue::Type::kObject || key == nullptr) return nullptr;
    for (size_t i = object.keys.size(); i > 0; --i) if (object.keys[i - 1] == key) return &object.values[i - 1];
    return nullptr;
}

bool char_has(const CharJsonValue& object, const char* key) {
    if (object.type != CharJsonValue::Type::kObject) return false;
    for (const auto& member : object.keys) if (member == key) return true;
    return false;
}

// 复刻 Android JSONObject.optXxx 语义：Double.parseDouble(trim 后字符串)，向零截断并饱和窄化。
// Kotlin 迁移前先校验字段；C++ 若按解析失败拒绝，会新增 400 并破坏逐字行为一致性。
bool char_parse_android_double(const std::string& text, double* out) {
    if (out == nullptr) return false;
    size_t begin = 0;
    size_t end_index = text.size();
    while (begin < end_index && static_cast<unsigned char>(text[begin]) <= 0x20) ++begin;
    while (end_index > begin && static_cast<unsigned char>(text[end_index - 1]) <= 0x20) {
        --end_index;
    }
    if (begin == end_index) return false;
    std::string trimmed = text.substr(begin, end_index - begin);
    const bool special = trimmed == "NaN" || trimmed == "+NaN" || trimmed == "-NaN" ||
                         trimmed == "Infinity" || trimmed == "+Infinity" ||
                         trimmed == "-Infinity";
    if (!special && !trimmed.empty() &&
        (trimmed.back() == 'f' || trimmed.back() == 'F' || trimmed.back() == 'd' ||
         trimmed.back() == 'D')) {
        trimmed.pop_back();
        if (trimmed.empty()) return false;
    }
    const size_t sign_offset = !trimmed.empty() &&
                                       (trimmed.front() == '+' || trimmed.front() == '-')
                                   ? 1
                                   : 0;
    if (trimmed.size() >= sign_offset + 2 && trimmed[sign_offset] == '0' &&
        (trimmed[sign_offset + 1] == 'x' || trimmed[sign_offset + 1] == 'X') &&
        trimmed.find_first_of("pP") == std::string::npos) {
        return false;
    }
    char* end = nullptr;
    const double parsed = std::strtod(trimmed.c_str(), &end);
    if (end == trimmed.c_str() || *end != '\0' ||
        ((std::isnan(parsed) || std::isinf(parsed)) && !special)) {
        return false;
    }
    *out = parsed;
    return true;
}

int64_t char_narrow_long(double value) {
    constexpr double kLongLower = -9223372036854775808.0;
    constexpr double kLongUpperExclusive = 9223372036854775808.0;
    if (std::isnan(value)) return 0;
    if (value <= kLongLower) return std::numeric_limits<int64_t>::min();
    if (value >= kLongUpperExclusive) return std::numeric_limits<int64_t>::max();
    return static_cast<int64_t>(value);
}

int32_t char_narrow_int(double value) {
    if (std::isnan(value)) return 0;
    if (value <= static_cast<double>(std::numeric_limits<int32_t>::min())) {
        return std::numeric_limits<int32_t>::min();
    }
    if (value >= static_cast<double>(std::numeric_limits<int32_t>::max())) {
        return std::numeric_limits<int32_t>::max();
    }
    return static_cast<int32_t>(value);
}

bool char_parse_android_number(const CharJsonValue* value, double* out) {
    if (value == nullptr || out == nullptr ||
        (value->type != CharJsonValue::Type::kNumber &&
         value->type != CharJsonValue::Type::kString)) {
        return false;
    }
    return char_parse_android_double(value->scalar, out);
}

int64_t char_opt_int(const CharJsonValue* value, int64_t fallback) {
    double parsed = 0;
    return char_parse_android_number(value, &parsed) ? char_narrow_int(parsed) : fallback;
}

int64_t char_opt_long(const CharJsonValue* value, int64_t fallback) {
    double parsed = 0;
    return char_parse_android_number(value, &parsed) ? char_narrow_long(parsed) : fallback;
}

bool char_int_field(const CharJsonValue& body, const char* key, int64_t* out) {
    if (out == nullptr || !char_has(body, key)) return false;
    *out = char_opt_int(char_member(body, key), -1);
    return true;
}

bool char_long_field(const CharJsonValue& body, const char* key, int64_t* out) {
    if (out == nullptr || !char_has(body, key)) return false;
    *out = char_opt_long(char_member(body, key), -1);
    return true;
}

bool char_bool_field(const CharJsonValue& body, const char* key, bool* out) {
    if (out == nullptr || !char_has(body, key)) return false;
    const CharJsonValue* value = char_member(body, key);
    if (value != nullptr && value->type == CharJsonValue::Type::kBool) {
        *out = value->boolean;
        return true;
    }
    if (value != nullptr && value->type == CharJsonValue::Type::kString) {
        std::string normalized = value->scalar;
        for (char& c : normalized) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if (normalized == "true") {
            *out = true;
            return true;
        }
        if (normalized == "false") {
            *out = false;
            return true;
        }
    }
    *out = false;
    return true;
}

bool char_path_int(const contracts::HttpReq* request, const char* name, int32_t* out) {
    if (request == nullptr || name == nullptr || out == nullptr || request->path_params == nullptr) {
        return false;
    }
    const size_t name_len = std::strlen(name);
    for (uint32_t i = 0; i < request->path_param_count; ++i) {
        const contracts::HttpPathParam& param = request->path_params[i];
        if (param.name == nullptr || param.name_len != name_len) continue;
        if (std::strncmp(param.name, name, name_len) != 0) continue;
        if (param.value == nullptr || param.value_len == 0) return false;
        int64_t value = 0;
        for (uint32_t k = 0; k < param.value_len; ++k) {
            const char c = param.value[k];
            if (c < '0' || c > '9') return false;
            value = value * 10 + (c - '0');
        }
        *out = static_cast<int32_t>(value);
        return true;
    }
    return false;
}

// 属性名 → 0..4（strength/agility/vitality/intelligence/spirit）；纯数字直接返回，无法识别返回 -1。
int32_t char_attr_index(const std::string& key) {
    if (key == "strength") return 0;
    if (key == "agility") return 1;
    if (key == "vitality") return 2;
    if (key == "intelligence") return 3;
    if (key == "spirit") return 4;
    if (key == "0") return 0;
    if (key == "1") return 1;
    if (key == "2") return 2;
    if (key == "3") return 3;
    if (key == "4") return 4;
    return -1;
}

struct CharWriteRoute {
    const char* path;
    contracts::QolCharacterWriteKind kind;
};

constexpr CharWriteRoute kCharWriteRoutes[] = {
    {"/api/op/character/{role}/experience", contracts::QolCharacterWriteKind::kSetExperience},
    {"/api/op/character/{role}/hp", contracts::QolCharacterWriteKind::kSetHp},
    {"/api/op/character/{role}/level", contracts::QolCharacterWriteKind::kSetLevel},
    {"/api/op/character/{role}/mp", contracts::QolCharacterWriteKind::kSetMp},
    {"/api/op/character/{role}/set_attr", contracts::QolCharacterWriteKind::kSetAttr},
    {"/api/op/character/{role}/status-point", contracts::QolCharacterWriteKind::kSetStatusPoint},
    {"/api/op/party/swap", contracts::QolCharacterWriteKind::kPartySwap},
    {"/api/character/combat/{role}/attack_target",
     contracts::QolCharacterWriteKind::kAttackTarget},
    {"/api/character/combat/{role}/cast_skill", contracts::QolCharacterWriteKind::kCastSkill},
    {"/api/character/combat/{role}/set_auto_attack",
     contracts::QolCharacterWriteKind::kSetAutoAttack},
    {"/api/character/combat/{role}/stop_combat", contracts::QolCharacterWriteKind::kStopCombat},
    {"/api/character/combat/switch_player", contracts::QolCharacterWriteKind::kSwitchPlayer},
    {"/api/character/grow/add_skill", contracts::QolCharacterWriteKind::kLearnAction},
    {"/api/character/grow/reset_skill", contracts::QolCharacterWriteKind::kSkillReset},
    {"/api/character/grow/reset_stat", contracts::QolCharacterWriteKind::kStatReset},
    {"/api/character/grow/{role}/add_stat", contracts::QolCharacterWriteKind::kAddStat},
    {"/api/character/party/include", contracts::QolCharacterWriteKind::kPartyInclude},
    {"/api/character/party/exclude", contracts::QolCharacterWriteKind::kPartyExclude},
    {"/api/character/party/discharge", contracts::QolCharacterWriteKind::kPartyDischarge},
    {"/api/character/party/withdraw", contracts::QolCharacterWriteKind::kPartyWithdraw},
};

// 解析 `{"stats":{...}}` 或 `{"attrs":{...}}` 为定长点集；键可为属性名或数字索引。
bool char_parse_points(const CharJsonValue& body, const char* object_key, CharWritePayload* payload,
                       std::string* error) {
    const CharJsonValue* points = char_member(body, object_key);
    const bool add = std::strcmp(object_key, "attrs") == 0;
    const char* missing = add
        ? "attrs required (object: strength/agility/vitality/intelligence/spirit 或 0-4 → 数量)"
        : "stats required (object: 属性名/索引 → 值)";
    if (points == nullptr || points->type != CharJsonValue::Type::kObject) { *error = missing; return false; }
    if (points->keys.empty()) { *error = add ? "attrs empty" : "stats empty"; return false; }
    if (points->keys.size() > 128) { *error = "bad request"; return false; }
    for (size_t i = 0; i < points->keys.size(); ++i) {
        const std::string& key = points->keys[i];
        const int32_t index = char_attr_index(key);
        if (index < 0) { *error = "bad attr: " + key + " (0-4 或 strength/agility/vitality/intelligence/spirit)"; return false; }
        const int64_t value = char_opt_int(&points->values[i], -1);
        if ((add && value <= 0) || (!add && (value < 0 || value > 255))) {
            *error = add ? "bad value for " + key + " (must be positive)" : "bad value for " + key + " (0-255)";
            return false;
        }
        if (payload->point_count >= static_cast<int32_t>(sizeof(payload->point_idx) / sizeof(payload->point_idx[0]))) {
            *error = "bad request";
            return false;
        }
        payload->point_idx[payload->point_count] = index;
        payload->point_amt[payload->point_count] = static_cast<int32_t>(value);
        ++payload->point_count;
    }
    return true;
}

QolResult char_write_command(const contracts::HttpReq* request, contracts::HttpRes* response,
                             void* ctx) {
    if (request == nullptr || response == nullptr || ctx == nullptr) {
        return response_error(response, 400, "bad request");
    }
    const auto kind = static_cast<contracts::QolCharacterWriteKind>(
        *static_cast<const contracts::QolCharacterWriteKind*>(ctx));
    const std::string body_text(request->body != nullptr
                                    ? reinterpret_cast<const char*>(request->body)
                                    : "",
                                request->body_len);
    const bool no_body = kind == contracts::QolCharacterWriteKind::kStatReset ||
                         kind == contracts::QolCharacterWriteKind::kSkillReset ||
                         kind == contracts::QolCharacterWriteKind::kStopCombat;
    CharJsonValue body{};
    if (!no_body && (!char_parse_json(body_text, &body) || body.type != CharJsonValue::Type::kObject)) {
        return response_error(response, 400, "bad request");
    }
    CharWritePayload payload{};
    payload.kind = static_cast<int32_t>(kind);
    switch (kind) {
        case contracts::QolCharacterWriteKind::kSetExperience: {
            if (!char_path_int(request, "role", &payload.i0)) {
                return response_error(response, 400, "role required");
            }
            int64_t exp = 0;
            if (!char_long_field(body, "exp", &exp) || exp < 0) {
                return response_error(response, 400, "exp required");
            }
            payload.i64 = exp;
            break;
        }
        case contracts::QolCharacterWriteKind::kSetHp:
        case contracts::QolCharacterWriteKind::kSetMp:
        case contracts::QolCharacterWriteKind::kSetStatusPoint: {
            if (!char_path_int(request, "role", &payload.i0)) {
                return response_error(response, 400, "role required");
            }
            int64_t value = 0;
            const char* key = kind == contracts::QolCharacterWriteKind::kSetHp ? "hp"
                : (kind == contracts::QolCharacterWriteKind::kSetMp ? "mp" : "points");
            if (!char_int_field(body, key, &value) || value < 0) {
                return response_error(response, 400, kind == contracts::QolCharacterWriteKind::kSetHp ? "hp required" :
                                      (kind == contracts::QolCharacterWriteKind::kSetMp ? "mp required" : "points required"));
            }
            payload.i1 = static_cast<int32_t>(value);
            break;
        }
        case contracts::QolCharacterWriteKind::kSetLevel: {
            if (!char_path_int(request, "role", &payload.i0)) {
                return response_error(response, 400, "role required");
            }
            int64_t level = 0;
            if (!char_has(body, "level")) {
                return response_error(response, 400, "level required");
            }
            level = char_opt_int(char_member(body, "level"), 0);
            bool force = false;
            (void)char_bool_field(body, "force", &force);
            if (!force && (level < 1 || level > 105)) {
                return response_error(response, 400, "level 1-105 (game max); force=true 跳过限制");
            }
            payload.i1 = static_cast<int32_t>(level);
            payload.i2 = force ? 1 : 0;
            break;
        }
        case contracts::QolCharacterWriteKind::kSetAttr: {
            if (!char_path_int(request, "role", &payload.i0)) {
                return response_error(response, 400, "role required");
            }
            std::string error;
            if (!char_parse_points(body, "stats", &payload, &error)) {
                return response_error(response, 400, error.c_str());
            }
            break;
        }
        case contracts::QolCharacterWriteKind::kPartySwap: {
            int64_t a = 0;
            int64_t b = 0;
            if (!char_has(body, "a") || !char_has(body, "b")) {
                return response_error(response, 400, "a and b required");
            }
            a = char_opt_int(char_member(body, "a"), -1);
            b = char_opt_int(char_member(body, "b"), -1);
            if (a < 0 || b < 0) return response_error(response, 400, "a and b required");
            payload.i0 = static_cast<int32_t>(a);
            payload.i1 = static_cast<int32_t>(b);
            break;
        }
        case contracts::QolCharacterWriteKind::kAttackTarget: {
            if (!char_path_int(request, "role", &payload.i0)) {
                return response_error(response, 400, "role required");
            }
            int64_t target = 0;
            if (!char_int_field(body, "target_slot", &target) || target < 0) {
                return response_error(response, 400, "target_slot required");
            }
            payload.i1 = static_cast<int32_t>(target);
            break;
        }
        case contracts::QolCharacterWriteKind::kCastSkill: {
            if (!char_path_int(request, "role", &payload.i0)) {
                return response_error(response, 400, "role required");
            }
            int64_t action = 0;
            if (!char_int_field(body, "action_id", &action) || action < 0) {
                return response_error(response, 400, "action_id required");
            }
            payload.i1 = static_cast<int32_t>(action);
            break;
        }
        case contracts::QolCharacterWriteKind::kSetAutoAttack:
        case contracts::QolCharacterWriteKind::kStopCombat: {
            if (!char_path_int(request, "role", &payload.i0)) {
                return response_error(response, 400, "role required");
            }
            if (kind == contracts::QolCharacterWriteKind::kSetAutoAttack) {
                bool on = false;
                if (!char_bool_field(body, "on", &on)) {
                    return response_error(response, 400, "on required");
                }
                payload.i1 = on ? 1 : 0;
            }
            break;
        }
        case contracts::QolCharacterWriteKind::kSwitchPlayer: {
            int64_t slot = 0;
            if (!char_int_field(body, "slot", &slot) || slot < 0 || slot > 2) {
                return response_error(response, 400, "slot required (0-2)");
            }
            payload.i0 = static_cast<int32_t>(slot);
            break;
        }
        case contracts::QolCharacterWriteKind::kLearnAction: {
            // 迁移前 Kotlin 对 add_skill 硬编码 role=0（角色面板固定主角）。
            payload.i0 = 0;
            int64_t action = 0;
            if (!char_int_field(body, "action_id", &action) || action < 0) {
                return response_error(response, 400, "action_id required");
            }
            int64_t level = 1;
            if (char_has(body, "level")) level = char_opt_int(char_member(body, "level"), 1);
            payload.i1 = static_cast<int32_t>(action);
            payload.i2 = static_cast<int32_t>(level);
            break;
        }
        case contracts::QolCharacterWriteKind::kStatReset:
        case contracts::QolCharacterWriteKind::kSkillReset: {
            payload.i0 = 0;
            break;
        }
        case contracts::QolCharacterWriteKind::kAddStat: {
            if (!char_path_int(request, "role", &payload.i0)) {
                return response_error(response, 400, "role required");
            }
            std::string error;
            if (!char_parse_points(body, "attrs", &payload, &error)) {
                return response_error(response, 400, error.c_str());
            }
            break;
        }
        case contracts::QolCharacterWriteKind::kPartyInclude:
        case contracts::QolCharacterWriteKind::kPartyExclude:
        case contracts::QolCharacterWriteKind::kPartyDischarge: {
            int64_t merc = 0;
            if (!char_int_field(body, "mercenary_slot", &merc) || merc < 0) {
                return response_error(response, 400, "mercenary_slot required");
            }
            payload.i0 = static_cast<int32_t>(merc);
            break;
        }
        case contracts::QolCharacterWriteKind::kPartyWithdraw: {
            int64_t merc = 0;
            int64_t equip = 0;
            if (!char_int_field(body, "mercenary_slot", &merc) || merc < 0 ||
                !char_int_field(body, "equip_slot", &equip) || equip < 0) {
                return response_error(response, 400, "mercenarySlot/equip_slot required");
            }
            payload.i0 = static_cast<int32_t>(merc);
            payload.i1 = static_cast<int32_t>(equip);
            break;
        }
        default:
            return response_error(response, 400, "unsupported command");
    }
    if (g_sdk == nullptr || g_sdk->commands.post_typed == nullptr) {
        return response_error(response, 503, "command port unavailable");
    }
    contracts::QolCommandTicket ticket = 0;
    if (!contracts::succeeded(g_sdk->commands.post_typed(
            kCharWriteHandlerId, &payload, sizeof(payload), &ticket)) ||
        ticket == 0) {
        return response_error(response, 503, "command queue unavailable");
    }
    const std::string out = std::string("{\"ok\":true,\"ticket\":") + std::to_string(ticket) + "}";
    return response_body(response, out.c_str(), static_cast<uint32_t>(out.size()));
}

// 失败体与迁移前 op_err 同构：{"ok":false,"error":"<原 op_err 文案>"}（不转义）。
bool write_op_error(char* out, uint32_t capacity, uint32_t* out_size, const char* message) {
    const std::string body = std::string("{\"ok\":false,\"error\":\"") +
                             (message != nullptr ? message : "character write failed") + "\"}";
    if (body.size() > capacity) return false;
    std::memcpy(out, body.data(), body.size());
    *out_size = static_cast<uint32_t>(body.size());
    return false;
}

// 写后 state：owner 经 character.state 读端口当场同步读取并序列化，与迁移前 build_*_json 逐字节一致。
bool append_character_state(std::string& body, contracts::QolCharacterStateKind kind,
                            uint32_t capacity) {
    std::vector<char> buffer(kPartyCapacity);
    uint32_t state_size = 0;
    const QolResult result = write_character_state_json(
        kind, buffer.data(), static_cast<uint32_t>(buffer.size()), &state_size);
    if (!contracts::succeeded(result)) return false;
    body = "{\"ok\":true,\"state\":";
    body.append(buffer.data(), state_size);
    body += "}";
    return body.size() <= capacity;
}

// 主线程结果处理器：owner 侧按 kind 分派 typed 低层原语并组装响应
// （成功 {"ok":true[,"state":...]}；失败透错误体）。
bool char_write_result(void*, const uint8_t* payload, uint32_t size, char* out, uint32_t capacity,
                       uint32_t* out_size) {
    if (out_size != nullptr) *out_size = 0;
    if (payload == nullptr || size != sizeof(CharWritePayload) ||
        g_character_write_ops == nullptr || out == nullptr || out_size == nullptr) {
        return false;
    }
    CharWritePayload p{};
    std::memcpy(&p, payload, size);
    const auto kind = static_cast<contracts::QolCharacterWriteKind>(p.kind);
    contracts::QolCharacterWriteOpResult op{};
    const auto* ops = g_character_write_ops;

    // 批量 kind：逐点下发（set_attr 单点一次，add_stat 按数量重复），组装保持在本 owner 内。
    if (kind == contracts::QolCharacterWriteKind::kSetAttr ||
        kind == contracts::QolCharacterWriteKind::kAddStat) {
        const bool set_attr = kind == contracts::QolCharacterWriteKind::kSetAttr;
        std::string body = set_attr ? "{\"ok\":true,\"set\":[" : "{\"ok\":true,\"applied\":[";
        bool first = true;
        for (int32_t i = 0; i < p.point_count; ++i) {
            const int32_t repetitions = set_attr ? 1 : p.point_amt[i];
            for (int32_t point = 0; point < repetitions; ++point) {
                const QolResult result =
                    set_attr ? ops->set_attr(p.i0, p.point_idx[i], p.point_amt[i], &op)
                             : ops->add_stat(p.i0, p.point_idx[i], &op);
                if (!contracts::succeeded(result)) {
                    if (set_attr) {
                        const std::string error =
                            std::string("{\"ok\":false,\"error\":\"set attr ") +
                            std::to_string(p.point_idx[i]) + " failed\"}";
                        if (error.size() > capacity) return false;
                        std::memcpy(out, error.data(), error.size());
                        *out_size = static_cast<uint32_t>(error.size());
                        return false;
                    }
                    return write_op_error(out, capacity, out_size,
                                          op.error_message_len != 0 ? op.error_message : nullptr);
                }
                if (!first) body += ",";
                first = false;
                body += std::string("{\"attr\":") + std::to_string(p.point_idx[i]);
                if (set_attr) {
                    body += std::string(",\"value\":") + std::to_string(p.point_amt[i]);
                }
                body += "}";
                if (body.size() > capacity) return false;
            }
        }
        body += "]}";
        if (body.size() > capacity) return false;
        std::memcpy(out, body.data(), body.size());
        *out_size = static_cast<uint32_t>(body.size());
        return true;
    }

    contracts::QolCharacterStateKind state_kind = contracts::QolCharacterStateKind::kParty;
    bool has_state = true;
    QolResult result = contracts::ok();
    switch (kind) {
        case contracts::QolCharacterWriteKind::kSetHp:
            result = ops->set_hp(p.i0, p.i1, &op);
            has_state = false;
            break;
        case contracts::QolCharacterWriteKind::kSetMp:
            result = ops->set_mp(p.i0, p.i1, &op);
            has_state = false;
            break;
        case contracts::QolCharacterWriteKind::kSetLevel:
            result = ops->set_level(p.i0, p.i1, p.i2, &op);
            has_state = false;
            break;
        case contracts::QolCharacterWriteKind::kSetExperience:
            result = ops->set_experience(p.i0, p.i64, &op);
            has_state = false;
            break;
        case contracts::QolCharacterWriteKind::kSetStatusPoint:
            result = ops->set_status_point(p.i0, p.i1, &op);
            has_state = false;
            break;
        case contracts::QolCharacterWriteKind::kPartySwap:
            result = ops->party_swap(p.i0, p.i1, &op);
            has_state = false;
            break;
        case contracts::QolCharacterWriteKind::kSetAutoAttack:
            result = ops->set_auto_attack(p.i0, p.i1, &op);
            state_kind = contracts::QolCharacterStateKind::kParty;
            break;
        case contracts::QolCharacterWriteKind::kCastSkill:
            result = ops->cast_skill(p.i0, p.i1, &op);
            state_kind = contracts::QolCharacterStateKind::kParty;
            break;
        case contracts::QolCharacterWriteKind::kAttackTarget:
            result = ops->attack_target(p.i0, p.i1, &op);
            state_kind = contracts::QolCharacterStateKind::kParty;
            break;
        case contracts::QolCharacterWriteKind::kSwitchPlayer:
            result = ops->switch_player(p.i0, &op);
            state_kind = contracts::QolCharacterStateKind::kPlayer;
            break;
        case contracts::QolCharacterWriteKind::kStopCombat:
            result = ops->stop_combat(p.i0, &op);
            state_kind = contracts::QolCharacterStateKind::kPlayer;
            break;
        case contracts::QolCharacterWriteKind::kLearnAction:
            result = ops->learn_action(p.i0, p.i1, p.i2, &op);
            state_kind = contracts::QolCharacterStateKind::kSkills;
            break;
        case contracts::QolCharacterWriteKind::kStatReset:
            result = ops->stat_reset(p.i0, &op);
            state_kind = contracts::QolCharacterStateKind::kPlayer;
            break;
        case contracts::QolCharacterWriteKind::kSkillReset:
            result = ops->skill_reset(p.i0, &op);
            state_kind = contracts::QolCharacterStateKind::kPlayer;
            break;
        case contracts::QolCharacterWriteKind::kPartyInclude:
            result = ops->party_include(p.i0, &op);
            state_kind = contracts::QolCharacterStateKind::kParty;
            break;
        case contracts::QolCharacterWriteKind::kPartyExclude:
            result = ops->party_exclude(p.i0, &op);
            state_kind = contracts::QolCharacterStateKind::kParty;
            break;
        case contracts::QolCharacterWriteKind::kPartyDischarge:
            result = ops->party_discharge(p.i0, &op);
            state_kind = contracts::QolCharacterStateKind::kParty;
            break;
        case contracts::QolCharacterWriteKind::kPartyWithdraw:
            result = ops->party_withdraw(p.i0, p.i1, &op);
            state_kind = contracts::QolCharacterStateKind::kParty;
            break;
        default:
            return false;
    }
    if (!contracts::succeeded(result)) {
        return write_op_error(out, capacity, out_size,
                              op.error_message_len != 0 ? op.error_message : nullptr);
    }
    std::string body = "{\"ok\":true}";
    if (has_state && !append_character_state(body, state_kind, capacity)) return false;
    if (body.size() > capacity) return false;
    std::memcpy(out, body.data(), body.size());
    *out_size = static_cast<uint32_t>(body.size());
    return true;
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
    // 写面 20 条（声明式 POST；owner 命令 + 票据）。
    {contracts::HttpMethod::kPost, kCharWriteRoutes[0].path, kPluginId, &char_write_command, const_cast<contracts::QolCharacterWriteKind*>(&kCharWriteRoutes[0].kind)},
    {contracts::HttpMethod::kPost, kCharWriteRoutes[1].path, kPluginId, &char_write_command, const_cast<contracts::QolCharacterWriteKind*>(&kCharWriteRoutes[1].kind)},
    {contracts::HttpMethod::kPost, kCharWriteRoutes[2].path, kPluginId, &char_write_command, const_cast<contracts::QolCharacterWriteKind*>(&kCharWriteRoutes[2].kind)},
    {contracts::HttpMethod::kPost, kCharWriteRoutes[3].path, kPluginId, &char_write_command, const_cast<contracts::QolCharacterWriteKind*>(&kCharWriteRoutes[3].kind)},
    {contracts::HttpMethod::kPost, kCharWriteRoutes[4].path, kPluginId, &char_write_command, const_cast<contracts::QolCharacterWriteKind*>(&kCharWriteRoutes[4].kind)},
    {contracts::HttpMethod::kPost, kCharWriteRoutes[5].path, kPluginId, &char_write_command, const_cast<contracts::QolCharacterWriteKind*>(&kCharWriteRoutes[5].kind)},
    {contracts::HttpMethod::kPost, kCharWriteRoutes[6].path, kPluginId, &char_write_command, const_cast<contracts::QolCharacterWriteKind*>(&kCharWriteRoutes[6].kind)},
    {contracts::HttpMethod::kPost, kCharWriteRoutes[7].path, kPluginId, &char_write_command, const_cast<contracts::QolCharacterWriteKind*>(&kCharWriteRoutes[7].kind)},
    {contracts::HttpMethod::kPost, kCharWriteRoutes[8].path, kPluginId, &char_write_command, const_cast<contracts::QolCharacterWriteKind*>(&kCharWriteRoutes[8].kind)},
    {contracts::HttpMethod::kPost, kCharWriteRoutes[9].path, kPluginId, &char_write_command, const_cast<contracts::QolCharacterWriteKind*>(&kCharWriteRoutes[9].kind)},
    {contracts::HttpMethod::kPost, kCharWriteRoutes[10].path, kPluginId, &char_write_command, const_cast<contracts::QolCharacterWriteKind*>(&kCharWriteRoutes[10].kind)},
    {contracts::HttpMethod::kPost, kCharWriteRoutes[11].path, kPluginId, &char_write_command, const_cast<contracts::QolCharacterWriteKind*>(&kCharWriteRoutes[11].kind)},
    {contracts::HttpMethod::kPost, kCharWriteRoutes[12].path, kPluginId, &char_write_command, const_cast<contracts::QolCharacterWriteKind*>(&kCharWriteRoutes[12].kind)},
    {contracts::HttpMethod::kPost, kCharWriteRoutes[13].path, kPluginId, &char_write_command, const_cast<contracts::QolCharacterWriteKind*>(&kCharWriteRoutes[13].kind)},
    {contracts::HttpMethod::kPost, kCharWriteRoutes[14].path, kPluginId, &char_write_command, const_cast<contracts::QolCharacterWriteKind*>(&kCharWriteRoutes[14].kind)},
    {contracts::HttpMethod::kPost, kCharWriteRoutes[15].path, kPluginId, &char_write_command, const_cast<contracts::QolCharacterWriteKind*>(&kCharWriteRoutes[15].kind)},
    {contracts::HttpMethod::kPost, kCharWriteRoutes[16].path, kPluginId, &char_write_command, const_cast<contracts::QolCharacterWriteKind*>(&kCharWriteRoutes[16].kind)},
    {contracts::HttpMethod::kPost, kCharWriteRoutes[17].path, kPluginId, &char_write_command, const_cast<contracts::QolCharacterWriteKind*>(&kCharWriteRoutes[17].kind)},
    {contracts::HttpMethod::kPost, kCharWriteRoutes[18].path, kPluginId, &char_write_command, const_cast<contracts::QolCharacterWriteKind*>(&kCharWriteRoutes[18].kind)},
    {contracts::HttpMethod::kPost, kCharWriteRoutes[19].path, kPluginId, &char_write_command, const_cast<contracts::QolCharacterWriteKind*>(&kCharWriteRoutes[19].kind)},
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
    g_character_write_ops = nullptr;
    g_character_state_port = nullptr;
    g_frame_task = 0;
    CharacterSnapshot empty_snapshot{};
    g_snapshot.publish(empty_snapshot);
    if (ctx.struct_size < contracts::PLUGIN_CONTEXT_PORTS_STRUCT_SIZE ||
        ctx.sdk_major != contracts::kHostSdkMajor || g_sdk == nullptr || ctx.ports == nullptr ||
        ctx.port_count != 4 || ctx.ports[0] == nullptr || ctx.ports[1] == nullptr ||
        ctx.ports[2] == nullptr || ctx.ports[3] == nullptr) {
        g_sdk = nullptr;
        return 1;
    }
    g_port = static_cast<const contracts::QolCharacterPort*>(ctx.ports[0]);
    g_http_port = static_cast<const contracts::QolCharacterHttpPort*>(ctx.ports[1]);
    g_character_write_ops =
        static_cast<const contracts::QolCharacterWriteOpsPort*>(ctx.ports[2]);
    g_character_state_port =
        static_cast<const contracts::QolCharacterStatePort*>(ctx.ports[3]);
    const bool ops_ready =
        g_character_write_ops != nullptr && g_character_write_ops->set_hp != nullptr &&
        g_character_write_ops->set_mp != nullptr && g_character_write_ops->set_level != nullptr &&
        g_character_write_ops->set_experience != nullptr &&
        g_character_write_ops->set_attr != nullptr &&
        g_character_write_ops->set_status_point != nullptr &&
        g_character_write_ops->party_swap != nullptr &&
        g_character_write_ops->set_auto_attack != nullptr &&
        g_character_write_ops->cast_skill != nullptr &&
        g_character_write_ops->attack_target != nullptr &&
        g_character_write_ops->switch_player != nullptr &&
        g_character_write_ops->stop_combat != nullptr && g_character_write_ops->learn_action != nullptr &&
        g_character_write_ops->add_stat != nullptr && g_character_write_ops->stat_reset != nullptr &&
        g_character_write_ops->skill_reset != nullptr && g_character_write_ops->party_include != nullptr &&
        g_character_write_ops->party_exclude != nullptr && g_character_write_ops->party_discharge != nullptr &&
        g_character_write_ops->party_withdraw != nullptr;
    if (!contracts::satisfies(g_port->abi_version, {1, 0}) || g_port->read.type == nullptr ||
        g_port->read.is_active_player_group == nullptr ||
        !contracts::satisfies(g_http_port->abi_version, {1, 0}) ||
        g_http_port->read.body_json == nullptr ||
        !contracts::satisfies(g_character_write_ops->abi_version,
                              {contracts::kQolPortCharacterWriteOpsVersion.major,
                               contracts::kQolPortCharacterWriteOpsVersion.minor}) ||
        !ops_ready ||
        !contracts::satisfies(g_character_state_port->abi_version,
                              {contracts::kQolPortCharacterStateVersion.major,
                               contracts::kQolPortCharacterStateVersion.minor}) ||
        g_character_state_port->read.party == nullptr ||
        g_character_state_port->read.player == nullptr ||
        g_character_state_port->read.skills == nullptr) {
        g_port = nullptr;
        g_http_port = nullptr;
        g_character_write_ops = nullptr;
        g_character_state_port = nullptr;
        g_sdk = nullptr;
        return 1;
    }
    // 写命令结果处理器：需要宿主尾部的票据结果面（QolHostSdk 尾部追加，按 abi 协商）。
    if (g_sdk->abi.size < sizeof(contracts::QolHostSdk) || g_sdk->abi.minor < 9 ||
        g_sdk->ticket_result.register_typed_result_handler == nullptr ||
        !contracts::succeeded(g_sdk->ticket_result.register_typed_result_handler(
            kCharWriteHandlerId, &char_write_result, nullptr))) {
        g_port = nullptr;
        g_http_port = nullptr;
        g_character_write_ops = nullptr;
        g_character_state_port = nullptr;
        g_sdk = nullptr;
        return 1;
    }
    log_message(g_sdk, "[core.character] provider install");
    return 0;
}

void plugin_deactivate(contracts::PluginContext& ctx) {
    stop_frame_task(ctx.sdk);
    log_message(ctx.sdk, "[core.character] provider deactivate");
    if (g_sdk != nullptr && g_sdk->ticket_result.unregister_typed_result_handler != nullptr) {
        (void)g_sdk->ticket_result.unregister_typed_result_handler(kCharWriteHandlerId);
    }
    g_port = nullptr;
    g_http_port = nullptr;
    g_character_write_ops = nullptr;
    g_character_state_port = nullptr;
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
    2,
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
    4,
    kHttpQueries,
    static_cast<uint32_t>(sizeof(kHttpQueries) / sizeof(kHttpQueries[0])),
};

}  // namespace

const contracts::PluginDescriptor& plugin_descriptor() {
    return kDescriptor;
}

}  // namespace qol::features::core_character
