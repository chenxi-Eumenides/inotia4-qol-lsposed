#include "contracts/character_facts.h"
#include "contracts/character_state.h"
#include "contracts/game_ports/character_http_port.h"
#include "contracts/game_ports/character_port.h"
#include "contracts/game_ports/character_state_port.h"
#include "contracts/game_ports/character_write_port.h"
#include "contracts/http.h"
#include "contracts/host_sdk.h"
#include "game/access/game_access.h"
#include "game/state/game_state.h"
#include "game_character.h"
#include "game_party.h"
#include "game_world.h"
#include "infra/capabilities.h"
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
static uint32_t g_state_party_reads = 0;
static uint32_t g_state_player_reads = 0;
static uint32_t g_state_skills_reads = 0;
static bool g_state_not_in_game = false;
static qol::contracts::QolCharacterStateParty g_live_party{};
static qol::contracts::QolCharacterStatePlayer g_live_player{};
static qol::contracts::QolCharacterStateSkills g_live_skills{};
static uint8_t g_legacy_members[3][0x400]{};
static bool g_legacy_member_present[3]{};
static uint8_t g_legacy_skill_node[0x20]{};
static uint8_t g_legacy_item[0x40]{};
static uint8_t g_legacy_option_node[0x20]{};
static bool g_legacy_item_present = false;
static uint16_t g_legacy_active_quest = 0;
static uint8_t g_legacy_leader_slot = 0;
static char g_legacy_name[qol::contracts::kQolCharacterStateNameCapacity]{};

uintptr_t g_base = 0;
void* g_active_quest = &g_legacy_active_quest;
void* g_main_merc_slot = &g_legacy_leader_slot;

#define CHECK(cond) do { \
    if (cond) ++g_pass; \
    else { ++g_fail; std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } \
} while (0)

namespace {

int legacy_party_size() { return g_live_player.party_count; }
int64_t legacy_money() { return g_live_player.money; }
uint32_t legacy_map_id() { return static_cast<uint32_t>(g_live_player.map_id); }
void* legacy_member(int role) {
    return role >= 0 && role < 3 && g_legacy_member_present[role]
               ? g_legacy_members[role]
               : nullptr;
}
char* legacy_name(void*) { return g_legacy_name; }
void* legacy_equip(void*, int slot) {
    return g_legacy_item_present && slot == 0 ? g_legacy_item : nullptr;
}
int legacy_item_bit(int, int, int) { return 12; }
int legacy_item_count(void*) { return 1; }
int legacy_item_rarity(void*) { return 3; }
int legacy_item_damage(void*) { return 4; }
int legacy_item_defense(void*) { return 5; }
int legacy_item_level(void*) { return 7; }
int32_t legacy_status_point(void*) { return g_live_party.members[0].status_point; }
int32_t legacy_stat_base(void*, int index) {
    return g_live_party.members[0].base_stats[index];
}
void append_legacy_item_attrs(std::string&, void*) {}

void sync_legacy_fixture() {
    std::memset(g_legacy_members, 0, sizeof(g_legacy_members));
    std::memset(g_legacy_member_present, 0, sizeof(g_legacy_member_present));
    std::memset(g_legacy_skill_node, 0, sizeof(g_legacy_skill_node));
    std::memset(g_legacy_item, 0, sizeof(g_legacy_item));
    std::memset(g_legacy_option_node, 0, sizeof(g_legacy_option_node));
    std::memset(g_legacy_name, 0, sizeof(g_legacy_name));
    g_legacy_item_present = false;
    g_legacy_active_quest = static_cast<uint16_t>(g_live_player.active_quest);
    g_legacy_leader_slot = static_cast<uint8_t>(g_live_player.leader_slot);
    for (int role = 0; role < 3; ++role) {
        if (g_live_party.member_present[role] == 0) continue;
        g_legacy_member_present[role] = true;
        auto* bytes = g_legacy_members[role];
        const auto& member = g_live_party.members[role];
        reinterpret_cast<int8_t*>(bytes)[C_TYPE] = static_cast<int8_t>(member.type);
        reinterpret_cast<int8_t*>(bytes)[C_CLASS] = static_cast<int8_t>(member.class_idx);
        *reinterpret_cast<uint16_t*>(bytes + C_NAME_ID) = static_cast<uint16_t>(member.name_id);
        reinterpret_cast<int8_t*>(bytes)[C_LEVEL] = static_cast<int8_t>(member.level);
        *reinterpret_cast<int32_t*>(bytes + C_HP) = member.hp;
        *reinterpret_cast<int32_t*>(bytes + C_MP) = member.mp;
        *reinterpret_cast<int32_t*>(bytes + C_EXP) = member.exp;
        *reinterpret_cast<int16_t*>(bytes + C_POS_X) = static_cast<int16_t>(g_live_player.x);
        *reinterpret_cast<int16_t*>(bytes + C_POS_Y) = static_cast<int16_t>(g_live_player.y);
        for (int i = 0; i < 32; ++i) {
            *reinterpret_cast<int32_t*>(bytes + C_ATTR + i * sizeof(int32_t)) = member.stats[i];
        }
        *reinterpret_cast<uint16_t*>(bytes + C_SKILL_BMP) =
            static_cast<uint16_t>(g_live_skills.members[role].unlock_bitmap);
        reinterpret_cast<int8_t*>(bytes)[C_SKILL_POINTS] =
            static_cast<int8_t>(g_live_skills.members[role].skill_points);
        if (g_live_skills.member_present[role] != 0 &&
            g_live_skills.members[role].skill_count > 0) {
            const auto& skill = g_live_skills.members[role].skills[0];
            *reinterpret_cast<uint16_t*>(g_legacy_skill_node + S_ACTION_ID) =
                static_cast<uint16_t>(skill.action_id);
            g_legacy_skill_node[S_LEVEL] = static_cast<uint8_t>(skill.level);
            *reinterpret_cast<uint8_t**>(g_legacy_skill_node + S_NEXT) = nullptr;
            *reinterpret_cast<uint8_t**>(bytes + C_SKILL_LIST) = g_legacy_skill_node;
        }
        for (uint32_t slot = 0; slot < qol::contracts::kQolCharacterStateEquipSlots; ++slot) {
            if (g_live_party.member_present[role] == 0 || member.equipment_present[slot] == 0 ||
                role != 0 || slot != 0) {
                continue;
            }
            const auto& item = member.equipment[slot];
            *reinterpret_cast<uint16_t*>(g_legacy_item + I_TYPE) =
                static_cast<uint16_t>(item.type_flags);
            g_legacy_item[I_MAGIC_RATE] = static_cast<uint8_t>(item.magic_rate);
            g_legacy_item[I_SOCKET] = static_cast<uint8_t>(item.socket);
            *reinterpret_cast<uint16_t*>(g_legacy_item + I_ENCHANT) =
                static_cast<uint16_t>(item.enchant);
            *reinterpret_cast<uint32_t*>(g_legacy_item + I_COUNT) =
                static_cast<uint32_t>(item.chaos_level | (item.chaos_rate << 8));
            *reinterpret_cast<uint16_t*>(g_legacy_option_node + O_INDEX) =
                static_cast<uint16_t>(item.options[0].id | (item.options[0].type << 13));
            *reinterpret_cast<int16_t*>(g_legacy_option_node + O_VALUE) =
                static_cast<int16_t>(item.options[0].value);
            *reinterpret_cast<uint8_t**>(g_legacy_item + I_OPTION_LIST) =
                item.option_count == 0 ? nullptr : g_legacy_option_node;
            g_legacy_item_present = true;
        }
        const uint32_t copy_size = member.name_size < sizeof(g_legacy_name) - 1
                                       ? member.name_size
                                       : static_cast<uint32_t>(sizeof(g_legacy_name) - 1);
        std::memcpy(g_legacy_name, member.name, copy_size);
    }
}

}  // namespace

GetMoneyFn fn_get_money = &legacy_money;
GetMemberFn fn_get_member = &legacy_member;
GetPartySizeFn fn_get_party_size = &legacy_party_size;
GetEquipFn fn_get_equip = &legacy_equip;
GetBitFn fn_get_bit = &legacy_item_bit;
GetCumulateCountFn fn_get_cumulate_count = &legacy_item_count;
GetRarityFn fn_get_rarity = &legacy_item_rarity;
GetItemStatFn fn_get_damage = &legacy_item_damage;
GetItemStatFn fn_get_defense = &legacy_item_defense;
ItemGetAbilityLevelFn fn_item_get_ability_level = &legacy_item_level;
GetNameFn fn_get_name = &legacy_name;
GetStatusPointFn fn_get_status_point = &legacy_status_point;
GetAttrFn2 fn_get_stat_base = nullptr;
GetActMaxLevelFn fn_get_act_max_level = nullptr;

bool game_in_world() { return !g_state_not_in_game; }
uint32_t current_map_id() { return legacy_map_id(); }
void* lead_member() { return legacy_member(0); }
void append_position(std::string& out, void* member) {
    if (member == nullptr) {
        out += ",\"x\":-1,\"y\":-1";
        return;
    }
    auto* bytes = static_cast<uint8_t*>(member);
    out += ",\"x\":" + std::to_string(*reinterpret_cast<int16_t*>(bytes + C_POS_X));
    out += ",\"y\":" + std::to_string(*reinterpret_cast<int16_t*>(bytes + C_POS_Y));
}
int32_t char_max_hp(const void*) { return g_live_party.members[0].max_hp; }
int32_t char_max_mp(const void*) { return g_live_party.members[0].max_mp; }
int32_t char_stat_total(const void*, int index) {
    return index >= 0 && index < 5 ? g_live_party.members[0].main_stats[index] : 0;
}
int64_t char_next_exp_cached(const void*) { return g_live_party.members[0].exp_next; }
bool item_is_equip(void*) { return true; }
void append_item_attrs(std::string& out, void* item) {
    auto* bytes = static_cast<uint8_t*>(item);
    if (fn_get_damage != nullptr) out += ",\"damage\":" + std::to_string(fn_get_damage(item));
    if (fn_get_defense != nullptr) out += ",\"defense\":" + std::to_string(fn_get_defense(item));
    out += ",\"magic_rate\":" + std::to_string(bytes[I_MAGIC_RATE]);
    if (fn_item_get_ability_level != nullptr) {
        out += ",\"need_level\":" + std::to_string(fn_item_get_ability_level(item));
    }
    const uint8_t socket = bytes[I_SOCKET];
    out += ",\"socket\":" + std::to_string(socket);
    out += ",\"socket_filled\":" + std::to_string(socket & 0x0F);
    out += ",\"socket_total\":" + std::to_string((socket >> 4) & 0x0F);
    const uint16_t enchant = *reinterpret_cast<const uint16_t*>(bytes + I_ENCHANT);
    out += ",\"enchant\":" + std::to_string(enchant);
    out += (enchant & 1) != 0 ? ",\"chaos\":true" : ",\"chaos\":false";
    out += ",\"enchant_id\":" + std::to_string((enchant >> 11) & 0x1F);
    out += ",\"enchant_level\":" + std::to_string((enchant >> 6) & 0x1F);
    const uint32_t count = *reinterpret_cast<const uint32_t*>(bytes + I_COUNT);
    out += ",\"chaos_level\":" + std::to_string(count & 0xFF);
    out += ",\"chaos_rate\":" + std::to_string((count >> 8) & 0xFF);
    auto* option = *reinterpret_cast<uint8_t**>(bytes + I_OPTION_LIST);
    out += ",\"options\":[";
    for (uint32_t i = 0; option != nullptr && i < 32; ++i) {
        if (i != 0) out.push_back(',');
        out += std::to_string(*reinterpret_cast<int16_t*>(option + O_VALUE));
        option = *reinterpret_cast<uint8_t**>(option + O_NEXT);
    }
    out += "],\"option_ids\":[";
    option = *reinterpret_cast<uint8_t**>(bytes + I_OPTION_LIST);
    for (uint32_t i = 0; option != nullptr && i < 32; ++i) {
        if (i != 0) out.push_back(',');
        out += std::to_string(*reinterpret_cast<uint16_t*>(option + O_INDEX) & 0x7F);
        option = *reinterpret_cast<uint8_t**>(option + O_NEXT);
    }
    out += "],\"option_types\":[";
    option = *reinterpret_cast<uint8_t**>(bytes + I_OPTION_LIST);
    for (uint32_t i = 0; option != nullptr && i < 32; ++i) {
        if (i != 0) out.push_back(',');
        out += std::to_string((*reinterpret_cast<uint16_t*>(option + O_INDEX) >> 13) & 0x7);
        option = *reinterpret_cast<uint8_t**>(option + O_NEXT);
    }
    out += "]";
}
std::string json_escape(const char* value) {
    std::string out;
    for (; value != nullptr && *value != '\0'; ++value) {
        const unsigned char ch = static_cast<unsigned char>(*value);
        switch (ch) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (ch < 0x20) {
                    char escaped[8]{};
                    std::snprintf(escaped, sizeof(escaped), "\\u%04x", ch);
                    out += escaped;
                } else {
                    out += static_cast<char>(ch);
                }
        }
    }
    return out;
}

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

QolResult fake_state_party(qol::contracts::QolCharacterStateParty* out) {
    ++g_state_party_reads;
    if (out == nullptr) return qol::contracts::error(kQolErrInvalidArgument, "bad out", 7);
    if (g_state_not_in_game) return qol::contracts::error(kQolErrNotReady, "not in game", 11);
    *out = g_live_party;
    return qol::contracts::ok();
}

QolResult fake_state_player(qol::contracts::QolCharacterStatePlayer* out) {
    ++g_state_player_reads;
    if (out == nullptr) return qol::contracts::error(kQolErrInvalidArgument, "bad out", 7);
    if (g_state_not_in_game) return qol::contracts::error(kQolErrNotReady, "not in game", 11);
    *out = g_live_player;
    return qol::contracts::ok();
}

QolResult fake_state_skills(qol::contracts::QolCharacterStateSkills* out) {
    ++g_state_skills_reads;
    if (out == nullptr) return qol::contracts::error(kQolErrInvalidArgument, "bad out", 7);
    *out = g_live_skills;
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
    ++g_live_player.money;
    return qol::contracts::ok();
}

bool fake_main_thread_result_path(const qol::contracts::CharacterStateApi* state_api,
                                 const qol::contracts::QolCharacterWriteArgs* args,
                                 char* out, uint32_t capacity, uint32_t* out_size) {
    char operation_result[128]{};
    uint32_t operation_size = 0;
    if (!qol::contracts::succeeded(fake_write(args, operation_result,
                                              sizeof(operation_result), &operation_size))) {
        return false;
    }
    return qol::contracts::succeeded(state_api->write_state_json(
        qol::contracts::QolCharacterStateKind::kPlayer, out, capacity, out_size));
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
    CHECK(descriptor.provides_count == 2 && descriptor.provides != nullptr);
    CHECK(std::strcmp(descriptor.provides[0].id,
                      qol::contracts::kCharacterFactsCapabilityId) == 0);
    CHECK(descriptor.provides[0].version.major == 1 && descriptor.provides[0].version.minor == 0);
    CHECK(descriptor.provides[0].iface != nullptr);
    CHECK(std::strcmp(descriptor.provides[1].id,
                      qol::contracts::kCharacterStateCapabilityId) == 0);
    CHECK(descriptor.provides[1].version.major == 1 && descriptor.provides[1].version.minor == 0);
    CHECK(descriptor.provides[1].iface != nullptr);
    CHECK(descriptor.port_count == 4 && descriptor.ports != nullptr);
    CHECK(descriptor.ports[0].id == qol::contracts::QolPortId::QOL_PORT_CHARACTER);
    CHECK(descriptor.ports[1].id == qol::contracts::QolPortId::QOL_PORT_CHARACTER_HTTP);
    CHECK(descriptor.ports[2].id == qol::contracts::QolPortId::QOL_PORT_CHARACTER_WRITE);
    CHECK(descriptor.ports[3].id ==
          qol::contracts::QolPortId::QOL_PORT_CHARACTER_STATE);

    const auto* api = static_cast<const qol::contracts::CharacterFactsApi*>(
        descriptor.provides[0].iface);
    CHECK(api->type != nullptr);
    CHECK(api->is_active_player_group != nullptr);
    const auto* state_api = static_cast<const qol::contracts::CharacterStateApi*>(
        descriptor.provides[1].iface);
    CHECK(state_api->write_state_json != nullptr);

    qol::contracts::QolHostSdk sdk{};
    sdk.abi = {sizeof(qol::contracts::QolHostSdk), qol::contracts::kHostSdkMajor,
               qol::contracts::kHostSdkMinor};
    sdk.commands.post_typed = &fake_post_typed;
    sdk.ticket_result.register_typed_result_handler = &fake_register_result;
    sdk.ticket_result.unregister_typed_result_handler = &fake_unregister_result;
    const qol::contracts::QolCharacterPort port{{1, 0}, {&fake_type, &fake_active}};
    const qol::contracts::QolCharacterHttpPort http_port{{1, 0}, {&fake_body_json}};
    const qol::contracts::QolCharacterWritePort write_port{{1, 0}, {&fake_write}};
    const qol::contracts::QolCharacterStatePort state_port{
        qol::contracts::kQolPortCharacterStateVersion,
        {&fake_state_party, &fake_state_player, &fake_state_skills}};
    const void* ports[] = {&port, &http_port, &write_port, &state_port};
    qol::contracts::PluginContext ctx{
        qol::contracts::PLUGIN_CONTEXT_STRUCT_SIZE,
        qol::contracts::kHostSdkMajor,
        qol::contracts::kHostSdkMinor,
        &sdk,
        nullptr,
        "core.character",
        ports,
        4,
    };

    CHECK(descriptor.install(ctx) == 0);
    CHECK(g_result_handler != nullptr);
    int32_t type_value = 0;
    int32_t active_value = 0;
    CHECK(api->type(reinterpret_cast<const void*>(1), &type_value).ok == kQolOk);
    CHECK(type_value == 7 && g_type_calls == 1);
    CHECK(api->is_active_player_group(reinterpret_cast<const void*>(1), &active_value).ok == kQolOk);
    CHECK(active_value == 1 && g_active_calls == 1);

    g_live_player = {100, 34, 3, 4, 8, 1, 2};
    char state_output[8192]{};
    uint32_t state_size = 0;
    CHECK(state_api->write_state_json(qol::contracts::QolCharacterStateKind::kPlayer,
                                     state_output, sizeof(state_output), &state_size).ok == kQolOk);
    CHECK(std::string(state_output, state_size) ==
          "{\"money\":100,\"map_id\":34,\"x\":3,\"y\":4,\"active_quest\":8,"
          "\"leader_slot\":1,\"party_count\":2}");
    constexpr char kPlayerGolden[] =
        "{\"money\":100,\"map_id\":34,\"x\":3,\"y\":4,\"active_quest\":8,"
        "\"leader_slot\":1,\"party_count\":2}";
    char exact_capacity_output[sizeof(kPlayerGolden) - 1]{};
    uint32_t exact_capacity_size = 0;
    CHECK(state_api->write_state_json(
              qol::contracts::QolCharacterStateKind::kPlayer, exact_capacity_output,
              static_cast<uint32_t>(sizeof(exact_capacity_output)), &exact_capacity_size).ok ==
          kQolOk);
    CHECK(exact_capacity_size == sizeof(exact_capacity_output));
    CHECK(std::string(exact_capacity_output, exact_capacity_size) == kPlayerGolden);

    g_live_party = {};
    g_live_party.member_present[0] = 1;
    auto& member = g_live_party.members[0];
    member.fields = qol::contracts::kQolCharacterStateHasName |
                    qol::contracts::kQolCharacterStateHasStatusPoint;
    member.type = 0;
    member.class_idx = 1;
    member.name_id = 7;
    member.level = 10;
    member.hp = 50;
    member.mp = 5;
    member.max_hp = 60;
    member.max_mp = 6;
    member.exp = 1;
    member.exp_next = 2;
    member.stats[0] = 1;
    member.main_stats[0] = 9;
    member.status_point = 3;
    member.name[0] = 'A';
    member.name[1] = '"';
    member.name[2] = '\n';
    member.name_size = 3;

    sync_legacy_fixture();
    state_size = 0;
    CHECK(state_api->write_state_json(qol::contracts::QolCharacterStateKind::kParty,
                                     state_output, sizeof(state_output), &state_size).ok == kQolOk);
    CHECK(std::string(state_output, state_size) == build_party_json());
    state_size = 0;
    CHECK(state_api->write_state_json(qol::contracts::QolCharacterStateKind::kPlayer,
                                     state_output, sizeof(state_output), &state_size).ok == kQolOk);
    CHECK(std::string(state_output, state_size) == build_player_json());

    member.equipment_present[0] = 1;
    auto& item = member.equipment[0];
    item.fields = qol::contracts::kQolCharacterStateItemHasCategory |
                  qol::contracts::kQolCharacterStateItemHasCount |
                  qol::contracts::kQolCharacterStateItemHasRarity |
                  qol::contracts::kQolCharacterStateItemHasDamage |
                  qol::contracts::kQolCharacterStateItemHasDefense |
                  qol::contracts::kQolCharacterStateItemHasNeedLevel;
    item.type_flags = 8;
    item.raw_rarity = 2;
    item.category = 12;
    item.count = 1;
    item.equip = 1;
    item.rarity = 3;
    item.damage = 4;
    item.defense = 5;
    item.magic_rate = 6;
    item.need_level = 7;
    item.socket = 33;
    item.socket_filled = 1;
    item.socket_total = 2;
    item.enchant = 1;
    item.chaos = 1;
    item.enchant_id = 0;
    item.enchant_level = 0;
    item.chaos_level = 9;
    item.chaos_rate = 10;
    item.option_count = 1;
    item.options[0] = {11, 12, 1};
    sync_legacy_fixture();

    std::string expected_party =
        "[{\"type\":0,\"class_idx\":1,\"name_id\":7,\"level\":10,\"hp\":50,\"mp\":5,"
        "\"max_hp\":60,\"max_mp\":6,\"exp\":1,\"exp_next\":2,\"stats\":{";
    for (int index = 0; index < 32; ++index) {
        if (index != 0) expected_party.push_back(',');
        expected_party += "\"" + std::to_string(index) + "\":" +
                          std::to_string(index == 0 ? 1 : 0);
    }
    expected_party += "},\"main_stats\":[9,0,0,0,0],\"status_point\":3,\"equipment\":["
                      "{\"slot\":0,\"type_flags\":8,\"raw_rarity\":2,\"category\":12,"
                      "\"count\":1,\"equip\":true,\"rarity\":3,\"damage\":4,\"defense\":5,"
                      "\"magic_rate\":6,\"need_level\":7,\"socket\":33,\"socket_filled\":1,"
                      "\"socket_total\":2,\"enchant\":1,\"chaos\":true,\"enchant_id\":0,"
                      "\"enchant_level\":0,\"chaos_level\":9,\"chaos_rate\":10,"
                      "\"options\":[11],\"option_ids\":[12],\"option_types\":[1]}";
    for (int slot = 1; slot < 10; ++slot) expected_party += ",null";
    expected_party += "],\"name\":\"A\\\"\\n\"},null,null]";
    state_size = 0;
    CHECK(state_api->write_state_json(qol::contracts::QolCharacterStateKind::kParty,
                                     state_output, sizeof(state_output), &state_size).ok == kQolOk);
    CHECK(std::string(state_output, state_size) == expected_party);
    CHECK(std::string(state_output, state_size) == build_party_json());

    g_live_skills = {};
    g_live_skills.member_present[0] = 1;
    auto& skills_member = g_live_skills.members[0];
    skills_member.skill_count = 1;
    skills_member.skills[0] = {7, 2, 0, 0};
    skills_member.unlock_bitmap = 9;
    skills_member.active_skill_id = -1;
    skills_member.skill_points = 3;
    sync_legacy_fixture();
    state_size = 0;
    CHECK(state_api->write_state_json(qol::contracts::QolCharacterStateKind::kSkills,
                                     state_output, sizeof(state_output), &state_size).ok == kQolOk);
    CHECK(std::string(state_output, state_size) ==
          "[{\"role\":0,\"skills\":[{\"action_id\":7,\"level\":2}],\"unlock_bitmap\":9,"
          "\"active_skill_id\":-1,\"skill_points\":3},null,null]");
    CHECK(std::string(state_output, state_size) == build_skills_json());

    g_state_not_in_game = true;
    state_size = 0;
    CHECK(state_api->write_state_json(qol::contracts::QolCharacterStateKind::kParty,
                                     state_output, sizeof(state_output), &state_size).ok == kQolOk);
    CHECK(std::string(state_output, state_size) == "{\"error\":\"not in game\"}");
    CHECK(std::string(state_output, state_size) == build_party_json());
    state_size = 0;
    CHECK(state_api->write_state_json(qol::contracts::QolCharacterStateKind::kPlayer,
                                     state_output, sizeof(state_output), &state_size).ok == kQolOk);
    CHECK(std::string(state_output, state_size) == build_player_json());
    state_size = 0;
    CHECK(state_api->write_state_json(qol::contracts::QolCharacterStateKind::kSkills,
                                     state_output, sizeof(state_output), &state_size).ok == kQolOk);
    CHECK(std::string(state_output, state_size) == build_skills_json());
    g_state_not_in_game = false;

    // Fake owner result path: execute a game write, then synchronously project live state.
    qol::contracts::QolCharacterWriteArgs immediate_write{};
    immediate_write.kind = static_cast<int32_t>(qol::contracts::QolCharacterWriteKind::kSetHp);
    state_size = 0;
    CHECK(fake_main_thread_result_path(state_api, &immediate_write, state_output,
                                       sizeof(state_output), &state_size));
    CHECK(std::string(state_output, state_size).find("\"money\":101") != std::string::npos);
    CHECK(g_state_player_reads >= 2);

    char tiny_output[4] = {'!', '!', '!', '!'};
    uint32_t tiny_size = 99;
    CHECK(state_api->write_state_json(qol::contracts::QolCharacterStateKind::kPlayer,
                                     tiny_output, sizeof(tiny_output), &tiny_size).code ==
          kQolErrBlocked);
    CHECK(tiny_size == 0 && tiny_output[0] == '!');

    qol::infra::capabilities_reset_for_test();
    const auto& state_provision = descriptor.provides[1];
    CHECK(qol::infra::register_capability({state_provision.id, state_provision.version,
                                           state_provision.iface}));
    CHECK(qol::infra::require_capability(qol::contracts::kCharacterStateCapabilityId,
                                         {1, 0}) == state_provision.iface);
    CHECK(qol::infra::unregister_capability(qol::contracts::kCharacterStateCapabilityId));
    CHECK(qol::infra::require_capability(qol::contracts::kCharacterStateCapabilityId,
                                         {1, 0}) == nullptr);
    qol::infra::capabilities_reset_for_test();

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
    CHECK(g_write_calls == 2);

    descriptor.deactivate(ctx);
    CHECK(g_result_handler == nullptr);
    CHECK(api->type(reinterpret_cast<const void*>(1), &type_value).code == kQolErrNotReady);
    CHECK(api->is_active_player_group(reinterpret_cast<const void*>(1), &active_value).code ==
          kQolErrNotReady);
    state_size = 99;
    CHECK(state_api->write_state_json(qol::contracts::QolCharacterStateKind::kPlayer,
                                     state_output, sizeof(state_output), &state_size).code ==
          kQolErrNotReady);
    CHECK(state_size == 0);
    CHECK(g_fail == 0);
    std::printf("character_facts_plugin: pass=%d fail=%d\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
