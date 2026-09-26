#include "contracts/character_facts.h"
#include "contracts/character_state.h"
#include "contracts/game_ports/character_http_port.h"
#include "contracts/game_ports/character_port.h"
#include "contracts/game_ports/character_state_port.h"
#include "contracts/game_ports/character_write_ops_port.h"
#include "contracts/game_ports/character_write_port.h"
#include "contracts/http.h"
#include "contracts/host_sdk.h"
#include "game/access/game_access.h"
#include "game/ops/character_state_read.h"
#include "game/ops/character_write.h"
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
static uint32_t g_state_party_reads = 0;
static uint32_t g_state_player_reads = 0;
static uint32_t g_state_skills_reads = 0;
static bool g_state_not_in_game = false;
// 写面 golden 需要 state 反映真实低层原语对 fake 角色内存的改动，走 game/ops 的 typed 读。
static bool g_state_reads_from_legacy = false;
// 真实 typed 原语的调用计数（批量族逐点次数断言）。
static int g_set_stat_base_calls = 0;
static int g_add_stat_main_calls = 0;
// 战斗族写后即时时序：非 0 时，fake 游戏写函数把该值写入角色 C_LEVEL，模拟原语对 live 数据的改动。
static int g_write_effect_level = 0;
// 队伍族（T2c-4c）可控 fake 面：佣兵槽 7 解析到的对象、装备槽 0 的 item，以及各原版链调用开关。
static uint8_t g_merc_object[0x400]{};
static uint8_t g_merc_item[0x40]{};
static int g_can_unequip_calls = 0;
static int g_can_unequip_result = 1;
static int g_unequip_calls = 0;
static int g_unequip_result = 1;
static int g_mercenary_release_calls = 0;
static int g_include_calls = 0;
static int g_include_result = 1;
static int g_exclude_calls = 0;
static int g_exclude_result = 1;
static bool g_leader_is_merc = false;
static bool g_merc_in_party = false;
static bool g_is_special_npc = false;
// 玩家族（T2c-4d）可控 fake 面：switch_player 的原版调用开关与 UI 占用桩。
static int g_set_active_player_calls = 0;
static int g_set_active_player_result = 1;
static const char* g_ui_blocked_message = nullptr;
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
    // 队伍族可控 fake：让佣兵对象顶替队长或已在队成员，覆盖 leader / already-in-party 分支。
    if (g_leader_is_merc && role == 0) return g_merc_object;
    if (g_merc_in_party && role == 1) return g_merc_object;
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

// 战斗族写后即时时序：fake 游戏写函数模拟对 live 角色内存的改动（同一结果处理器随后读 kParty）。
void apply_write_effect(void* ch) {
    if (g_write_effect_level != 0 && ch != nullptr) {
        reinterpret_cast<int8_t*>(ch)[C_LEVEL] = static_cast<int8_t>(g_write_effect_level);
    }
}

// 真实 typed 写原语依赖的其余 game 函数指针（fake 实现只满足成功/失败判定）。
void legacy_set_exp(void*, int32_t) {}
int legacy_set_level(void*, int32_t) { return 1; }
void legacy_set_status_point(void*, int32_t) {}
void legacy_set_stat_base(void*, int32_t, int32_t) { ++g_set_stat_base_calls; }
int legacy_get_stat_main(void*, int32_t) { return 0; }
void legacy_set_stat_main(void*, int32_t, int32_t) { ++g_add_stat_main_calls; }
void legacy_set_auto_attack(void* ch, int32_t) { apply_write_effect(ch); }
// 玩家族写后即时：fake 原版链模拟对主角 live 数据的改动（移动坐标），供随后读 kPlayer。
void apply_player_effect(void* ch) {
    if (g_write_effect_level == 0 || ch == nullptr) return;
    *reinterpret_cast<int16_t*>(static_cast<uint8_t*>(ch) + C_POS_X) =
        static_cast<int16_t>(g_write_effect_level);
}
// 技能族写后即时：fake 原版学习链模拟 live 技能数据改动（技能点/解锁位图），供随后读 kSkills。
void apply_skill_effect(void* ch) {
    if (g_write_effect_level == 0 || ch == nullptr) return;
    auto* bytes = static_cast<uint8_t*>(ch);
    reinterpret_cast<int8_t*>(bytes)[C_SKILL_POINTS] = 1;
    *reinterpret_cast<uint16_t*>(bytes + C_SKILL_BMP) = 0x1F;
}
void* legacy_learn_action(void* ch, int32_t, int32_t) {
    apply_skill_effect(ch);
    return nullptr;
}
void legacy_char_initialize_status(void* ch) { apply_player_effect(ch); }
void legacy_char_initialize_skill(void* ch) { apply_player_effect(ch); }
void* legacy_char_get_enemy_target(void*, int32_t, int32_t) { return legacy_member(0); }
void legacy_char_set_action_id(void* ch, int32_t, void*) { apply_write_effect(ch); }
void legacy_char_set_target(void* ch, void*) { apply_write_effect(ch); }
void legacy_char_stop_combat(void* ch) { apply_player_effect(ch); }
void legacy_party_swap(int32_t, int32_t) {}
// 队伍族写后即时：fake 原版链模拟对 live 队伍成员（slot 0）的改动，供同一结果处理器随后读 kParty。
void apply_party_effect() {
    if (g_write_effect_level == 0) return;
    void* member = legacy_member(0);
    if (member != nullptr) {
        reinterpret_cast<int8_t*>(member)[C_LEVEL] = static_cast<int8_t>(g_write_effect_level);
    }
}
int legacy_include_party(void*) { ++g_include_calls; apply_party_effect(); return g_include_result; }
int legacy_exclude_party(void*) { ++g_exclude_calls; apply_party_effect(); return g_exclude_result; }
void legacy_mercenary_release(int32_t) { ++g_mercenary_release_calls; apply_party_effect(); }
int legacy_unequip(void*, int32_t) { ++g_unequip_calls; apply_party_effect(); return g_unequip_result; }
int legacy_can_unequip(void*, void*) { ++g_can_unequip_calls; return g_can_unequip_result; }
int legacy_is_special_npc(void*) { return g_is_special_npc ? 1 : 0; }
int legacy_set_active_player(int32_t) {
    ++g_set_active_player_calls;
    return g_set_active_player_result;
}

SetExpFn fn_set_exp = &legacy_set_exp;
SetLevelFn fn_set_level = &legacy_set_level;
SetStatusPointFn fn_set_status_point = &legacy_set_status_point;
SetStatBaseFn fn_set_stat_base = &legacy_set_stat_base;
GetStatMainFn fn_get_stat_main = &legacy_get_stat_main;
SetStatMainFn fn_set_stat_main = &legacy_set_stat_main;
SetAutoAttackFn fn_set_auto_attack = &legacy_set_auto_attack;
LearnActionFn fn_learn_action = &legacy_learn_action;
CharInitializeStatusFn fn_char_initialize_status = &legacy_char_initialize_status;
CharInitializeSkillFn fn_char_initialize_skill = &legacy_char_initialize_skill;
CharGetEnemyTargetFn fn_char_get_enemy_target = &legacy_char_get_enemy_target;
CharSetActionIdFn fn_char_set_action_id = &legacy_char_set_action_id;
CharSetTargetFn fn_char_set_target = &legacy_char_set_target;
CharStopCombatFn fn_char_stop_combat = &legacy_char_stop_combat;
PartySwapFn fn_party_swap = &legacy_party_swap;
IncludePartyFn fn_include_party = &legacy_include_party;
ExcludePartyFn fn_exclude_party = &legacy_exclude_party;
MercenaryReleaseFn fn_mercenary_release = &legacy_mercenary_release;
UnequipFn fn_unequip = &legacy_unequip;
CanUnequipFn fn_can_unequip = &legacy_can_unequip;
IsSpecialNpcFn fn_is_special_npc = &legacy_is_special_npc;
SetActivePlayerFn fn_set_active_player = &legacy_set_active_player;

void* member_or_null(int role) { return legacy_member(role); }
// 佣兵槽 7 解析到一个不在队伍中的独立对象，用于 include/exclude/discharge/withdraw 成功路径。
void* find_char_by_merc_slot(int slot) {
    return slot == 7 ? g_merc_object : nullptr;
}
const char* ui_blocked() { return g_ui_blocked_message; }
void frame_cache_force_refresh() {}
bool pool_obj_valid(const uint8_t* obj) { return obj != nullptr; }

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
    if (g_state_reads_from_legacy) return qol::game::ops::read_character_party(out);
    *out = g_live_party;
    return qol::contracts::ok();
}

QolResult fake_state_player(qol::contracts::QolCharacterStatePlayer* out) {
    ++g_state_player_reads;
    if (out == nullptr) return qol::contracts::error(kQolErrInvalidArgument, "bad out", 7);
    if (g_state_not_in_game) return qol::contracts::error(kQolErrNotReady, "not in game", 11);
    if (g_state_reads_from_legacy) return qol::game::ops::read_character_player(out);
    *out = g_live_player;
    return qol::contracts::ok();
}

QolResult fake_state_skills(qol::contracts::QolCharacterStateSkills* out) {
    ++g_state_skills_reads;
    if (out == nullptr) return qol::contracts::error(kQolErrInvalidArgument, "bad out", 7);
    if (g_state_reads_from_legacy) return qol::game::ops::read_character_skills(out);
    *out = g_live_skills;
    return qol::contracts::ok();
}

// 注入真实 typed 写原语端口：owner 分派 → game/ops；state 经 typed 读获取。
const qol::contracts::QolCharacterWriteOpsPort kRealWriteOpsPort{
    qol::contracts::kQolPortCharacterWriteOpsVersion,
    &qol::game::ops::set_hp,
    &qol::game::ops::set_mp,
    &qol::game::ops::set_level,
    &qol::game::ops::set_experience,
    &qol::game::ops::set_attr,
    &qol::game::ops::set_status_point,
    &qol::game::ops::party_swap,
    &qol::game::ops::set_auto_attack,
    &qol::game::ops::cast_skill,
    &qol::game::ops::attack_target,
    &qol::game::ops::switch_player,
    &qol::game::ops::stop_combat,
    &qol::game::ops::learn_action,
    &qol::game::ops::add_stat,
    &qol::game::ops::stat_reset,
    &qol::game::ops::skill_reset,
    &qol::game::ops::party_include,
    &qol::game::ops::party_exclude,
    &qol::game::ops::party_discharge,
    &qol::game::ops::party_withdraw,
};

// 与 plugins/core.character 的私有载荷同构；golden 直接驱动 owner 结果处理器。
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

// 与 game_system.cpp 的 build_character_write_result_json 同构的 golden 基线：调用同一批
// data_op_* 与 build_*_json，按迁移前规则拼装。game_system.cpp 属 legacy 聚合 TU，不并入 host
// 测试；此处逐行复刻其信封逻辑，与 owner 输出比对。
std::string legacy_character_write_result_json(const CharWritePayloadMirror& p) {
    using qol::contracts::QolCharacterWriteKind;
    std::string op;
    std::string state;
    switch (static_cast<QolCharacterWriteKind>(p.kind)) {
        case QolCharacterWriteKind::kSetHp: op = data_op_set_hp(p.i0, p.i1); break;
        case QolCharacterWriteKind::kSetMp: op = data_op_set_mp(p.i0, p.i1); break;
        case QolCharacterWriteKind::kSetLevel:
            op = data_op_set_level(p.i0, p.i1, p.i2 != 0);
            break;
        case QolCharacterWriteKind::kSetExperience:
            op = data_op_set_experience(p.i0, p.i64);
            break;
        case QolCharacterWriteKind::kSetAttr: op = data_op_set_attr(p.i0, p.i1, p.i2); break;
        case QolCharacterWriteKind::kSetStatusPoint:
            op = data_op_set_status_point(p.i0, p.i1);
            break;
        case QolCharacterWriteKind::kPartySwap: op = data_op_party_swap(p.i0, p.i1); break;
        case QolCharacterWriteKind::kSetAutoAttack:
            op = data_op_set_auto_attack(p.i0, p.i1);
            state = build_party_json();
            break;
        case QolCharacterWriteKind::kCastSkill:
            op = data_op_cast(p.i0, p.i1);
            state = build_party_json();
            break;
        case QolCharacterWriteKind::kAttackTarget:
            op = data_op_attack(p.i0, p.i1);
            state = build_party_json();
            break;
        case QolCharacterWriteKind::kSwitchPlayer:
            op = data_op_switch_player(p.i0);
            state = build_player_json();
            break;
        case QolCharacterWriteKind::kStopCombat:
            op = data_op_stop_combat(p.i0);
            state = build_player_json();
            break;
        case QolCharacterWriteKind::kLearnAction:
            op = data_op_learn_action(p.i0, p.i1, p.i2);
            state = build_skills_json();
            break;
        case QolCharacterWriteKind::kAddStat: op = data_op_add_stat(p.i0, p.i1); break;
        case QolCharacterWriteKind::kStatReset:
            op = data_op_stat_reset(p.i0);
            state = build_player_json();
            break;
        case QolCharacterWriteKind::kSkillReset:
            op = data_op_skill_reset(p.i0);
            state = build_player_json();
            break;
        case QolCharacterWriteKind::kPartyInclude:
            op = data_op_include_party(p.i0);
            state = build_party_json();
            break;
        case QolCharacterWriteKind::kPartyExclude:
            op = data_op_exclude_party(p.i0);
            state = build_party_json();
            break;
        case QolCharacterWriteKind::kPartyDischarge:
            op = data_op_discharge(p.i0);
            state = build_party_json();
            break;
        case QolCharacterWriteKind::kPartyWithdraw:
            op = data_op_withdraw(p.i0, p.i1);
            state = build_party_json();
            break;
        default: return "";
    }
    const bool ok = op.find("\"ok\":false") == std::string::npos;
    std::string body = op;
    if (ok && !state.empty() && !op.empty() && op.back() == '}') {
        body = op.substr(0, op.size() - 1) + ",\"state\":" + state + "}";
    }
    return body;
}

bool run_owner_write(const CharWritePayloadMirror& payload, std::string* body) {
    char storage[8192]{};
    uint32_t size = 0;
    const bool ok = g_result_handler != nullptr &&
                    g_result_handler(nullptr, reinterpret_cast<const uint8_t*>(&payload),
                                     sizeof(payload), storage, sizeof(storage), &size);
    *body = std::string(storage, size);
    return ok;
}

// 写面 golden 的统一 fake 角色状态：单一角色、队伍 1 人、技能 1 条。
void setup_write_fixture() {
    g_live_party = {};
    g_live_party.member_present[0] = 1;
    auto& member = g_live_party.members[0];
    member.fields = qol::contracts::kQolCharacterStateHasName;
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
    member.status_point = 5;
    member.stats[0] = 1;
    member.main_stats[0] = 9;
    member.name[0] = 'A';
    member.name_size = 1;
    g_live_player = {100, 34, 3, 4, 8, 0, 1};
    g_live_skills = {};
    g_live_skills.member_present[0] = 1;
    g_live_skills.members[0].skill_count = 1;
    g_live_skills.members[0].skills[0] = {7, 2, 0, 0};
    g_live_skills.members[0].unlock_bitmap = 9;
    g_live_skills.members[0].active_skill_id = -1;
    g_live_skills.members[0].skill_points = 3;
}

// 18 个单点 kind 的响应逐字节对照 legacy builder；批量 kind 组装另断言（保持在 owner）。
void test_write_golden() {
    using qol::contracts::QolCharacterWriteKind;
    g_state_reads_from_legacy = true;

    setup_write_fixture();

    struct Case {
        QolCharacterWriteKind kind;
        int32_t i0;
        int32_t i1;
        int32_t i2;
        int64_t i64;
    };
    const Case single[] = {
        {QolCharacterWriteKind::kSetHp, 0, 50, 0, 0},
        {QolCharacterWriteKind::kSetMp, 0, 10, 0, 0},
        {QolCharacterWriteKind::kSetLevel, 0, 12, 1, 0},
        {QolCharacterWriteKind::kSetExperience, 0, 0, 0, 100},
        {QolCharacterWriteKind::kSetStatusPoint, 0, 5, 0, 0},
        {QolCharacterWriteKind::kPartySwap, 0, 1, 0, 0},
        {QolCharacterWriteKind::kSetAutoAttack, 0, 1, 0, 0},
        {QolCharacterWriteKind::kCastSkill, 0, 7, 0, 0},
        {QolCharacterWriteKind::kAttackTarget, 0, 0, 0, 0},
        {QolCharacterWriteKind::kSwitchPlayer, 1, 0, 0, 0},
        {QolCharacterWriteKind::kStopCombat, 0, 0, 0, 0},
        {QolCharacterWriteKind::kLearnAction, 0, 7, 2, 0},
        {QolCharacterWriteKind::kStatReset, 0, 0, 0, 0},
        {QolCharacterWriteKind::kSkillReset, 0, 0, 0, 0},
        {QolCharacterWriteKind::kPartyInclude, 7, 0, 0, 0},
        {QolCharacterWriteKind::kPartyExclude, 7, 0, 0, 0},
        {QolCharacterWriteKind::kPartyDischarge, 7, 0, 0, 0},
        {QolCharacterWriteKind::kPartyWithdraw, 7, 0, 0, 0},
    };

    for (const auto& test : single) {
        CharWritePayloadMirror payload{};
        payload.kind = static_cast<int32_t>(test.kind);
        payload.i0 = test.i0;
        payload.i1 = test.i1;
        payload.i2 = test.i2;
        payload.i64 = test.i64;
        sync_legacy_fixture();
        std::string owner;
        const bool owner_ok = run_owner_write(payload, &owner);
        sync_legacy_fixture();
        const std::string legacy = legacy_character_write_result_json(payload);
        CHECK(owner == legacy);
        CHECK(owner_ok == (legacy.find("\"ok\":false") == std::string::npos));
    }

    // 非 world 态：18 kind 统一失败体逐字对照（owner 不附 state）。
    g_state_not_in_game = true;
    for (const auto& test : single) {
        CharWritePayloadMirror payload{};
        payload.kind = static_cast<int32_t>(test.kind);
        payload.i0 = test.i0;
        payload.i1 = test.i1;
        payload.i2 = test.i2;
        payload.i64 = test.i64;
        std::string owner;
        const bool owner_ok = run_owner_write(payload, &owner);
        const std::string legacy = legacy_character_write_result_json(payload);
        CHECK(owner == legacy);
        CHECK(!owner_ok && owner == "{\"ok\":false,\"error\":\"not in game\"}");
    }
    g_state_not_in_game = false;

    // 批量 kind：组装保持在 core.character（不迁入 game/ops），逐字断言。
    CharWritePayloadMirror set_attr{};
    set_attr.kind = static_cast<int32_t>(QolCharacterWriteKind::kSetAttr);
    set_attr.i0 = 0;
    set_attr.point_count = 1;
    set_attr.point_idx[0] = 2;
    set_attr.point_amt[0] = 9;
    sync_legacy_fixture();
    std::string set_owner;
    CHECK(run_owner_write(set_attr, &set_owner));
    CHECK(set_owner == "{\"ok\":true,\"set\":[{\"attr\":2,\"value\":9}]}");

    CharWritePayloadMirror add_stat{};
    add_stat.kind = static_cast<int32_t>(QolCharacterWriteKind::kAddStat);
    add_stat.i0 = 0;
    add_stat.point_count = 1;
    add_stat.point_idx[0] = 2;
    add_stat.point_amt[0] = 2;
    sync_legacy_fixture();
    std::string add_owner;
    CHECK(run_owner_write(add_stat, &add_owner));
    CHECK(add_owner == "{\"ok\":true,\"applied\":[{\"attr\":2},{\"attr\":2}]}");

    g_state_not_in_game = true;
    std::string set_fail;
    CHECK(!run_owner_write(set_attr, &set_fail));
    CHECK(set_fail == "{\"ok\":false,\"error\":\"set attr 2 failed\"}");
    std::string add_fail;
    CHECK(!run_owner_write(add_stat, &add_fail));
    CHECK(add_fail == "{\"ok\":false,\"error\":\"not in game\"}");
    g_state_not_in_game = false;

    // 写后即时：op 改 fake 角色内存后，同一 owner 结果处理器读到新值（经 typed 读，不读周期快照）。
    CharWritePayloadMirror instant{};
    instant.kind = static_cast<int32_t>(QolCharacterWriteKind::kSwitchPlayer);
    instant.i0 = 2;
    sync_legacy_fixture();
    std::string instant_body;
    CHECK(run_owner_write(instant, &instant_body));
    CHECK(instant_body.find("\"leader_slot\":2") != std::string::npos);
    CHECK(instant_body.find("\"money\":100") != std::string::npos);

    g_state_reads_from_legacy = false;
}

// T2c-4a：无 state 数值/属性族（8 kind）typed 单路径族级覆盖。
// 6 单点 kind 与 legacy 逐字节对照且不含 state；批量 set_attr/add_stat 的组装与失败语义逐字断言。
void test_write_no_state_family() {
    using qol::contracts::QolCharacterWriteKind;
    g_state_reads_from_legacy = true;
    setup_write_fixture();
    // 该族无 state 附加：本段任何操作都不得读 character.state。
    const uint32_t state_reads_before =
        g_state_party_reads + g_state_player_reads + g_state_skills_reads;

    struct Case {
        QolCharacterWriteKind kind;
        int32_t i0;
        int32_t i1;
        int32_t i2;
        int64_t i64;
    };
    const Case singles[] = {
        {QolCharacterWriteKind::kSetHp, 0, 50, 0, 0},
        {QolCharacterWriteKind::kSetMp, 0, 10, 0, 0},
        {QolCharacterWriteKind::kSetLevel, 0, 12, 1, 0},
        {QolCharacterWriteKind::kSetExperience, 0, 0, 0, 100},
        {QolCharacterWriteKind::kSetStatusPoint, 0, 5, 0, 0},
        {QolCharacterWriteKind::kPartySwap, 0, 1, 0, 0},
    };

    // 成功体：typed 单路径 → {"ok":true}，无 state 附加，且与 legacy 基线逐字一致。
    for (const auto& test : singles) {
        CharWritePayloadMirror payload{};
        payload.kind = static_cast<int32_t>(test.kind);
        payload.i0 = test.i0;
        payload.i1 = test.i1;
        payload.i2 = test.i2;
        payload.i64 = test.i64;
        sync_legacy_fixture();
        std::string owner;
        const bool ok = run_owner_write(payload, &owner);
        sync_legacy_fixture();
        const std::string legacy = legacy_character_write_result_json(payload);
        CHECK(ok && owner == legacy && owner == "{\"ok\":true}");
        CHECK(owner.find("\"state\"") == std::string::npos);
    }

    // 非 world：失败体逐字、无 state、ticket 记失败。
    g_state_not_in_game = true;
    for (const auto& test : singles) {
        CharWritePayloadMirror payload{};
        payload.kind = static_cast<int32_t>(test.kind);
        payload.i0 = test.i0;
        payload.i1 = test.i1;
        payload.i2 = test.i2;
        payload.i64 = test.i64;
        std::string owner;
        const bool ok = run_owner_write(payload, &owner);
        const std::string legacy = legacy_character_write_result_json(payload);
        CHECK(!ok && owner == legacy && owner == "{\"ok\":false,\"error\":\"not in game\"}");
        CHECK(owner.find("\"state\"") == std::string::npos);
    }
    g_state_not_in_game = false;

    // role 边界：role=9 在结果处理器返回 role not found（HTTP 解析层不新增 400），逐字对照。
    const Case role_boundary[] = {
        {QolCharacterWriteKind::kSetHp, 9, 5, 0, 0},
        {QolCharacterWriteKind::kSetMp, 9, 5, 0, 0},
        {QolCharacterWriteKind::kSetLevel, 9, 12, 1, 0},
        {QolCharacterWriteKind::kSetExperience, 9, 0, 0, 100},
        {QolCharacterWriteKind::kSetStatusPoint, 9, 5, 0, 0},
    };
    for (const auto& test : role_boundary) {
        CharWritePayloadMirror payload{};
        payload.kind = static_cast<int32_t>(test.kind);
        payload.i0 = test.i0;
        payload.i1 = test.i1;
        payload.i2 = test.i2;
        payload.i64 = test.i64;
        std::string owner;
        const bool ok = run_owner_write(payload, &owner);
        const std::string legacy = legacy_character_write_result_json(payload);
        CHECK(!ok && owner == legacy && owner == "{\"ok\":false,\"error\":\"role not found\"}");
    }
    CharWritePayloadMirror swap_boundary{};
    swap_boundary.kind = static_cast<int32_t>(QolCharacterWriteKind::kPartySwap);
    swap_boundary.i0 = 3;
    swap_boundary.i1 = 0;
    std::string swap_body;
    CHECK(!run_owner_write(swap_boundary, &swap_body));
    CHECK(swap_body == legacy_character_write_result_json(swap_boundary) &&
          swap_body == "{\"ok\":false,\"error\":\"bad slot\"}");

    // 批量 set_attr：逐项设置；组装留 owner。
    CharWritePayloadMirror set_ok{};
    set_ok.kind = static_cast<int32_t>(QolCharacterWriteKind::kSetAttr);
    set_ok.i0 = 0;
    set_ok.point_count = 3;
    set_ok.point_idx[0] = 0;
    set_ok.point_amt[0] = 7;
    set_ok.point_idx[1] = 1;
    set_ok.point_amt[1] = 8;
    set_ok.point_idx[2] = 2;
    set_ok.point_amt[2] = 9;
    g_set_stat_base_calls = 0;
    sync_legacy_fixture();
    std::string set_ok_body;
    CHECK(run_owner_write(set_ok, &set_ok_body));
    CHECK(set_ok_body ==
          "{\"ok\":true,\"set\":[{\"attr\":0,\"value\":7},{\"attr\":1,\"value\":8},"
          "{\"attr\":2,\"value\":9}]}");
    CHECK(g_set_stat_base_calls == 3 && set_ok_body.find("\"state\"") == std::string::npos);

    // 批量 set_attr：中途失败包装为 set attr $idx failed，且只下发到失败点。
    CharWritePayloadMirror set_mid{};
    set_mid.kind = static_cast<int32_t>(QolCharacterWriteKind::kSetAttr);
    set_mid.i0 = 0;
    set_mid.point_count = 2;
    set_mid.point_idx[0] = 0;
    set_mid.point_amt[0] = 7;
    set_mid.point_idx[1] = 9;
    set_mid.point_amt[1] = 8;
    g_set_stat_base_calls = 0;
    sync_legacy_fixture();
    std::string set_mid_body;
    CHECK(!run_owner_write(set_mid, &set_mid_body));
    CHECK(set_mid_body == "{\"ok\":false,\"error\":\"set attr 9 failed\"}");
    CHECK(g_set_stat_base_calls == 1);

    // 批量 set_attr：role 边界同样包装（与迁移前 owner 语义一致）。
    CharWritePayloadMirror set_role{};
    set_role.kind = static_cast<int32_t>(QolCharacterWriteKind::kSetAttr);
    set_role.i0 = 9;
    set_role.point_count = 1;
    set_role.point_idx[0] = 0;
    set_role.point_amt[0] = 7;
    std::string set_role_body;
    CHECK(!run_owner_write(set_role, &set_role_body));
    CHECK(set_role_body == "{\"ok\":false,\"error\":\"set attr 0 failed\"}");

    // 批量 set_attr：非 world 首点失败透传包装体，ticket 记失败。
    g_state_not_in_game = true;
    std::string set_world_body;
    CHECK(!run_owner_write(set_ok, &set_world_body));
    CHECK(set_world_body == "{\"ok\":false,\"error\":\"set attr 0 failed\"}");
    g_state_not_in_game = false;

    // 批量 add_stat：按数量逐点 +1；首次失败透传原错误体，且只下发到失败点。
    CharWritePayloadMirror add_ok{};
    add_ok.kind = static_cast<int32_t>(QolCharacterWriteKind::kAddStat);
    add_ok.i0 = 0;
    add_ok.point_count = 1;
    add_ok.point_idx[0] = 2;
    add_ok.point_amt[0] = 3;
    g_add_stat_main_calls = 0;
    sync_legacy_fixture();
    std::string add_ok_body;
    CHECK(run_owner_write(add_ok, &add_ok_body));
    CHECK(add_ok_body == "{\"ok\":true,\"applied\":[{\"attr\":2},{\"attr\":2},{\"attr\":2}]}");
    CHECK(g_add_stat_main_calls == 3 && add_ok_body.find("\"state\"") == std::string::npos);

    CharWritePayloadMirror add_mid{};
    add_mid.kind = static_cast<int32_t>(QolCharacterWriteKind::kAddStat);
    add_mid.i0 = 0;
    add_mid.point_count = 2;
    add_mid.point_idx[0] = 0;
    add_mid.point_amt[0] = 1;
    add_mid.point_idx[1] = 9;
    add_mid.point_amt[1] = 1;
    g_add_stat_main_calls = 0;
    sync_legacy_fixture();
    std::string add_mid_body;
    CHECK(!run_owner_write(add_mid, &add_mid_body));
    CHECK(add_mid_body == "{\"ok\":false,\"error\":\"bad attr\"}");
    CHECK(g_add_stat_main_calls == 1);

    // 批量 add_stat：role 边界透传原错误体（与 legacy builder 单点一致）。
    CharWritePayloadMirror add_role{};
    add_role.kind = static_cast<int32_t>(QolCharacterWriteKind::kAddStat);
    add_role.i0 = 9;
    add_role.point_count = 1;
    add_role.point_idx[0] = 0;
    add_role.point_amt[0] = 1;
    std::string add_role_body;
    CHECK(!run_owner_write(add_role, &add_role_body));
    CHECK(add_role_body == "{\"ok\":false,\"error\":\"role not found\"}");

    // 批量 add_stat：非 world 首次失败透传，ticket 记失败且可回取错误体。
    g_state_not_in_game = true;
    g_add_stat_main_calls = 0;
    std::string add_world_body;
    CHECK(!run_owner_write(add_ok, &add_world_body));
    CHECK(add_world_body == "{\"ok\":false,\"error\":\"not in game\"}");
    CHECK(g_add_stat_main_calls == 0);
    g_state_not_in_game = false;

    CHECK(g_state_party_reads + g_state_player_reads + g_state_skills_reads == state_reads_before);
    g_state_reads_from_legacy = false;
}

// T2c-4b：战斗 party-state 族（kSetAutoAttack/kCastSkill/kAttackTarget）typed 单路径 +
// owner 自组装 kParty 投影（与 character.state capability 同一实现）覆盖。
// kAttackTarget 成功路径需 g_base 非 0 且池槽有效；用小型 fake 池避免依赖真实 VMA 布局。
void test_write_party_state_family(const qol::contracts::CharacterStateApi* state_api) {
    using qol::contracts::QolCharacterWriteKind;
    g_state_reads_from_legacy = true;
    setup_write_fixture();
    alignas(8) static uint8_t pool_base_slot[8]{};
    alignas(8) static uint8_t pool_object[64]{};
    *reinterpret_cast<uint8_t**>(pool_base_slot) = pool_object;
    const uintptr_t pool_g_base =
        reinterpret_cast<uintptr_t>(pool_base_slot) - G_CHAR_POOL_VMA;
    g_base = pool_g_base;

    struct Case {
        QolCharacterWriteKind kind;
        int32_t i0;
        int32_t i1;
        int32_t i2;
        int64_t i64;
    };
    const Case success[] = {
        {QolCharacterWriteKind::kSetAutoAttack, 0, 1, 0, 0},
        {QolCharacterWriteKind::kCastSkill, 0, 7, 0, 0},
        {QolCharacterWriteKind::kAttackTarget, 0, 0, 0, 0},
    };

    const auto make_payload = [](const Case& test) {
        CharWritePayloadMirror payload{};
        payload.kind = static_cast<int32_t>(test.kind);
        payload.i0 = test.i0;
        payload.i1 = test.i1;
        payload.i2 = test.i2;
        payload.i64 = test.i64;
        return payload;
    };
    const auto embedded_party = [](const std::string& owner, bool* found) {
        const std::string marker = ",\"state\":";
        const size_t at = owner.find(marker);
        if (at == std::string::npos) {
            *found = false;
            return std::string();
        }
        *found = true;
        return owner.substr(at + marker.size(), owner.size() - at - marker.size() - 1);
    };

    // 成功体：typed 单路径输出与 legacy 基线逐字节一致；内嵌 kParty 子树与 capability 逐字一致。
    for (const auto& test : success) {
        const CharWritePayloadMirror payload = make_payload(test);
        sync_legacy_fixture();
        std::string owner;
        const bool ok = run_owner_write(payload, &owner);
        bool found = false;
        const std::string embedded = embedded_party(owner, &found);
        char capability[8192]{};
        uint32_t capability_size = 0;
        const bool capability_ok =
            state_api != nullptr &&
            state_api->write_state_json(qol::contracts::QolCharacterStateKind::kParty, capability,
                                        sizeof(capability), &capability_size).ok == kQolOk;
        CHECK(found && capability_ok &&
              embedded == std::string(capability, capability_size));
        sync_legacy_fixture();
        const std::string legacy = legacy_character_write_result_json(payload);
        CHECK(ok && owner == legacy);
        CHECK(owner.find("\"state\":[") != std::string::npos);
    }

    // 写后即时：原语改 fake live 角色内存（level 10→42）后，同一结果处理器响应读到新值。
    g_write_effect_level = 42;
    for (const auto& test : success) {
        const CharWritePayloadMirror payload = make_payload(test);
        sync_legacy_fixture();
        std::string owner;
        CHECK(run_owner_write(payload, &owner));
        CHECK(owner.find("\"level\":42") != std::string::npos);
        CHECK(owner.find("\"level\":10") == std::string::npos);
    }
    g_write_effect_level = 0;

    // 非 world：失败体逐字、无 state、票据 result 可回取原错误体。
    g_state_not_in_game = true;
    for (const auto& test : success) {
        const CharWritePayloadMirror payload = make_payload(test);
        std::string owner;
        const bool ok = run_owner_write(payload, &owner);
        const std::string legacy = legacy_character_write_result_json(payload);
        CHECK(!ok && owner == legacy && owner == "{\"ok\":false,\"error\":\"not in game\"}");
        CHECK(owner.find("\"state\"") == std::string::npos);
    }
    g_state_not_in_game = false;

    // role 边界：结果处理器返回 role not found（HTTP 解析层不新增 400），逐字对照。
    const Case role_boundary[] = {
        {QolCharacterWriteKind::kSetAutoAttack, 9, 1, 0, 0},
        {QolCharacterWriteKind::kCastSkill, 9, 7, 0, 0},
        {QolCharacterWriteKind::kAttackTarget, 9, 0, 0, 0},
    };
    for (const auto& test : role_boundary) {
        const CharWritePayloadMirror payload = make_payload(test);
        std::string owner;
        const bool ok = run_owner_write(payload, &owner);
        const std::string legacy = legacy_character_write_result_json(payload);
        CHECK(!ok && owner == legacy && owner == "{\"ok\":false,\"error\":\"role not found\"}");
    }

    // 参数边界：cast 未学技能、attack 目标槽无效，均透传原错误体。
    CharWritePayloadMirror cast_unknown{};
    cast_unknown.kind = static_cast<int32_t>(QolCharacterWriteKind::kCastSkill);
    cast_unknown.i0 = 0;
    cast_unknown.i1 = 99;
    std::string cast_unknown_body;
    CHECK(!run_owner_write(cast_unknown, &cast_unknown_body));
    CHECK(cast_unknown_body == legacy_character_write_result_json(cast_unknown) &&
          cast_unknown_body == "{\"ok\":false,\"error\":\"skill not learned\"}");
    CharWritePayloadMirror attack_bad_target{};
    attack_bad_target.kind = static_cast<int32_t>(QolCharacterWriteKind::kAttackTarget);
    attack_bad_target.i0 = 0;
    attack_bad_target.i1 = C_CHARSYSTEM_POOL_SLOTS + 1;
    std::string attack_bad_target_body;
    CHECK(!run_owner_write(attack_bad_target, &attack_bad_target_body));
    CHECK(attack_bad_target_body == legacy_character_write_result_json(attack_bad_target) &&
          attack_bad_target_body == "{\"ok\":false,\"error\":\"target not found\"}");

    // 缺口补证：g_base==0 时 kAttackTarget 固定失败（旧 host 无法覆盖成功路径）；装池后同一
    // payload 走成功路径并附加 kParty，证明成功路径已被可控 fake 覆盖。
    g_base = 0;
    CharWritePayloadMirror attack_success{};
    attack_success.kind = static_cast<int32_t>(QolCharacterWriteKind::kAttackTarget);
    attack_success.i0 = 0;
    attack_success.i1 = 0;
    std::string no_pool_body;
    CHECK(!run_owner_write(attack_success, &no_pool_body) &&
          no_pool_body == "{\"ok\":false,\"error\":\"target not found\"}");
    g_base = pool_g_base;
    sync_legacy_fixture();
    std::string with_pool_body;
    CHECK(run_owner_write(attack_success, &with_pool_body) &&
          with_pool_body.find("\"state\":[") != std::string::npos);

    g_base = 0;
    g_state_reads_from_legacy = false;
}

// T2c-4c：队伍 party-state 族（kPartyInclude/kPartyExclude/kPartyDischarge/kPartyWithdraw）
// typed 单路径 + owner 自组装 kParty 投影覆盖。withdraw 必须走 fn_can_unequip 前检再 fn_unequip
// （game_party.cpp:73-77），discharge 保留 fn_mercenary_release 原版副作用（game_party.cpp:62），
// 两者以调用计数锁定；失败体逐字对照 legacy 基线，失败时不触发原版副作用。
void test_write_party_member_family(const qol::contracts::CharacterStateApi* state_api) {
    using qol::contracts::QolCharacterWriteKind;
    g_state_reads_from_legacy = true;
    setup_write_fixture();
    std::memset(g_merc_object, 0, sizeof(g_merc_object));
    // 装备槽 0 预置非空 item：withdraw 必须经 fn_can_unequip 前检。
    *reinterpret_cast<void**>(g_merc_object + C_EQUIP) = g_merc_item;
    g_can_unequip_calls = 0;
    g_can_unequip_result = 1;
    g_unequip_calls = 0;
    g_unequip_result = 1;
    g_mercenary_release_calls = 0;
    g_include_calls = 0;
    g_include_result = 1;
    g_exclude_calls = 0;
    g_exclude_result = 1;
    g_leader_is_merc = false;
    g_merc_in_party = false;
    g_is_special_npc = false;

    struct Case {
        QolCharacterWriteKind kind;
        int32_t i0;
        int32_t i1;
        int32_t i2;
        int64_t i64;
    };
    const auto make_payload = [](const Case& test) {
        CharWritePayloadMirror payload{};
        payload.kind = static_cast<int32_t>(test.kind);
        payload.i0 = test.i0;
        payload.i1 = test.i1;
        payload.i2 = test.i2;
        payload.i64 = test.i64;
        return payload;
    };
    const auto embedded_party = [](const std::string& owner, bool* found) {
        const std::string marker = ",\"state\":";
        const size_t at = owner.find(marker);
        if (at == std::string::npos) {
            *found = false;
            return std::string();
        }
        *found = true;
        return owner.substr(at + marker.size(), owner.size() - at - marker.size() - 1);
    };
    // 成功体：owner 侧计数在 legacy 基线运行前捕获，避免对照源自身的原版链调用污染次数。
    const auto run_golden = [&](const Case& test, int* owner_can_unequip, int* owner_unequip,
                                int* owner_release) {
        const CharWritePayloadMirror payload = make_payload(test);
        g_can_unequip_calls = 0;
        g_unequip_calls = 0;
        g_mercenary_release_calls = 0;
        sync_legacy_fixture();
        std::string owner;
        const bool ok = run_owner_write(payload, &owner);
        if (owner_can_unequip != nullptr) *owner_can_unequip = g_can_unequip_calls;
        if (owner_unequip != nullptr) *owner_unequip = g_unequip_calls;
        if (owner_release != nullptr) *owner_release = g_mercenary_release_calls;
        bool found = false;
        const std::string embedded = embedded_party(owner, &found);
        char capability[8192]{};
        uint32_t capability_size = 0;
        const bool capability_ok =
            state_api != nullptr &&
            state_api->write_state_json(qol::contracts::QolCharacterStateKind::kParty, capability,
                                        sizeof(capability), &capability_size).ok == kQolOk;
        CHECK(found && capability_ok && embedded == std::string(capability, capability_size));
        sync_legacy_fixture();
        const std::string legacy = legacy_character_write_result_json(payload);
        CHECK(ok && owner == legacy && owner.find("\"state\":[") != std::string::npos);
    };
    const auto run_failure = [&](const Case& test, const char* expected) {
        const CharWritePayloadMirror payload = make_payload(test);
        std::string owner;
        const bool ok = run_owner_write(payload, &owner);
        CHECK(!ok && owner == legacy_character_write_result_json(payload) &&
              owner == expected);
        CHECK(owner.find("\"state\"") == std::string::npos);
    };

    const Case success[] = {
        {QolCharacterWriteKind::kPartyInclude, 7, 0, 0, 0},
        {QolCharacterWriteKind::kPartyExclude, 7, 0, 0, 0},
        {QolCharacterWriteKind::kPartyDischarge, 7, 0, 0, 0},
        {QolCharacterWriteKind::kPartyWithdraw, 7, 0, 0, 0},
    };

    // 成功体：typed 单路径输出与 legacy 基线逐字节一致；内嵌 kParty 子树与 capability 同源。
    for (const auto& test : success) {
        int owner_can = 0;
        int owner_unequip = 0;
        int owner_release = 0;
        run_golden(test, &owner_can, &owner_unequip, &owner_release);
        if (test.kind == QolCharacterWriteKind::kPartyWithdraw) {
            // item 非空 → can_unequip 前检恰一次，通过后 fn_unequip 恰一次。
            CHECK(owner_can == 1 && owner_unequip == 1);
        }
        if (test.kind == QolCharacterWriteKind::kPartyDischarge) {
            // 原版遣散副作用 MERCENARYSYSTEM_Release 恰一次。
            CHECK(owner_release == 1);
        }
    }

    // 写后即时：原版链改 fake live 队伍成员（level 10→42）后，同一结果处理器响应读到新值。
    g_write_effect_level = 42;
    for (const auto& test : success) {
        const CharWritePayloadMirror payload = make_payload(test);
        sync_legacy_fixture();
        std::string owner;
        CHECK(run_owner_write(payload, &owner));
        CHECK(owner.find("\"level\":42") != std::string::npos);
        CHECK(owner.find("\"level\":10") == std::string::npos);
    }
    g_write_effect_level = 0;

    // 非 world：失败体逐字、无 state、票据 result 可回取原错误体。
    g_state_not_in_game = true;
    for (const auto& test : success) {
        run_failure(test, "{\"ok\":false,\"error\":\"not in game\"}");
    }
    g_state_not_in_game = false;

    // 佣兵槽无效（非 7）：mercenary not found。
    for (const auto& test : success) {
        Case missing = test;
        missing.i0 = 5;
        run_failure(missing, "{\"ok\":false,\"error\":\"mercenary not found\"}");
    }

    // withdraw 装备槽越界：bad slot（先于任何 can_unequip 调用）。
    g_can_unequip_calls = 0;
    run_failure({QolCharacterWriteKind::kPartyWithdraw, 7, C_EQUIP_SLOTS, 0, 0},
                "{\"ok\":false,\"error\":\"bad slot\"}");
    CHECK(g_can_unequip_calls == 0);

    // withdraw 前检拒绝：can_unequip 恰一次且不调 fn_unequip（game_party.cpp:75）。
    g_can_unequip_result = 0;
    g_can_unequip_calls = 0;
    g_unequip_calls = 0;
    {
        const CharWritePayloadMirror payload =
            make_payload({QolCharacterWriteKind::kPartyWithdraw, 7, 0, 0, 0});
        sync_legacy_fixture();
        std::string owner;
        const bool ok = run_owner_write(payload, &owner);
        CHECK(!ok && owner == "{\"ok\":false,\"error\":\"cannot unequip\"}");
        CHECK(g_can_unequip_calls == 1 && g_unequip_calls == 0);
        sync_legacy_fixture();
        CHECK(owner == legacy_character_write_result_json(payload));
    }
    g_can_unequip_result = 1;

    // withdraw 底层失败：fn_unequip 返回 0 → withdraw failed。
    g_unequip_result = 0;
    run_failure({QolCharacterWriteKind::kPartyWithdraw, 7, 0, 0, 0},
                "{\"ok\":false,\"error\":\"withdraw failed\"}");
    g_unequip_result = 1;

    // 队伍闸门：队长不可离队/遣散（先于原版副作用），任务 NPC 同理。
    g_leader_is_merc = true;
    g_mercenary_release_calls = 0;
    run_failure({QolCharacterWriteKind::kPartyExclude, 7, 0, 0, 0},
                "{\"ok\":false,\"error\":\"cannot exclude leader\"}");
    run_failure({QolCharacterWriteKind::kPartyDischarge, 7, 0, 0, 0},
                "{\"ok\":false,\"error\":\"cannot discharge leader\"}");
    CHECK(g_mercenary_release_calls == 0);
    g_leader_is_merc = false;

    g_is_special_npc = true;
    run_failure({QolCharacterWriteKind::kPartyExclude, 7, 0, 0, 0},
                "{\"ok\":false,\"error\":\"cannot exclude quest npc\"}");
    run_failure({QolCharacterWriteKind::kPartyDischarge, 7, 0, 0, 0},
                "{\"ok\":false,\"error\":\"cannot discharge quest npc\"}");
    g_is_special_npc = false;

    // include 闸门：已在队 / 队伍已满。
    g_merc_in_party = true;
    run_failure({QolCharacterWriteKind::kPartyInclude, 7, 0, 0, 0},
                "{\"ok\":false,\"error\":\"already in party\"}");
    g_merc_in_party = false;
    const int32_t saved_party_count = g_live_player.party_count;
    g_live_player.party_count = 3;
    run_failure({QolCharacterWriteKind::kPartyInclude, 7, 0, 0, 0},
                "{\"ok\":false,\"error\":\"party full\"}");
    g_live_player.party_count = saved_party_count;

    // 原版调用失败：include/exclude 返回 0 时透传原错误体。
    g_include_result = 0;
    run_failure({QolCharacterWriteKind::kPartyInclude, 7, 0, 0, 0},
                "{\"ok\":false,\"error\":\"party full or include failed\"}");
    g_include_result = 1;
    g_exclude_result = 0;
    run_failure({QolCharacterWriteKind::kPartyExclude, 7, 0, 0, 0},
                "{\"ok\":false,\"error\":\"exclude failed\"}");
    g_exclude_result = 1;

    g_state_reads_from_legacy = false;
}

// T2c-4d：player-state 族（kSwitchPlayer/kStopCombat/kStatReset/kSkillReset）typed 单路径 +
// owner 自组装 kPlayer 投影覆盖。switch_player 必须保留 fn_set_active_player 成功后的
// g_main_merc_slot 同步（game_party.cpp:86-89）；失败/越界时先返回、不得写槽，以桩状态与计数锁定。
void test_write_player_family(const qol::contracts::CharacterStateApi* state_api) {
    using qol::contracts::QolCharacterWriteKind;
    g_state_reads_from_legacy = true;
    setup_write_fixture();
    g_set_active_player_calls = 0;
    g_set_active_player_result = 1;
    g_ui_blocked_message = nullptr;

    struct Case {
        QolCharacterWriteKind kind;
        int32_t i0;
        int32_t i1;
        int32_t i2;
        int64_t i64;
    };
    const auto make_payload = [](const Case& test) {
        CharWritePayloadMirror payload{};
        payload.kind = static_cast<int32_t>(test.kind);
        payload.i0 = test.i0;
        payload.i1 = test.i1;
        payload.i2 = test.i2;
        payload.i64 = test.i64;
        return payload;
    };
    const auto embedded_player = [](const std::string& owner, bool* found) {
        const std::string marker = ",\"state\":";
        const size_t at = owner.find(marker);
        if (at == std::string::npos) {
            *found = false;
            return std::string();
        }
        *found = true;
        return owner.substr(at + marker.size(), owner.size() - at - marker.size() - 1);
    };
    // owner 侧调用计数与 leader 槽在 legacy 基线（也会调原版函数）之前捕获。
    const auto run_golden = [&](const Case& test, int* owner_calls, uint8_t* owner_leader) {
        const CharWritePayloadMirror payload = make_payload(test);
        g_set_active_player_calls = 0;
        sync_legacy_fixture();
        std::string owner;
        const bool ok = run_owner_write(payload, &owner);
        if (owner_calls != nullptr) *owner_calls = g_set_active_player_calls;
        if (owner_leader != nullptr) *owner_leader = g_legacy_leader_slot;
        bool found = false;
        const std::string embedded = embedded_player(owner, &found);
        char capability[8192]{};
        uint32_t capability_size = 0;
        const bool capability_ok =
            state_api != nullptr &&
            state_api->write_state_json(qol::contracts::QolCharacterStateKind::kPlayer, capability,
                                        sizeof(capability), &capability_size).ok == kQolOk;
        CHECK(found && capability_ok && embedded == std::string(capability, capability_size));
        sync_legacy_fixture();
        const std::string legacy = legacy_character_write_result_json(payload);
        CHECK(ok && owner == legacy && owner.find("\"state\":{") != std::string::npos);
    };
    const auto run_failure = [&](const Case& test, const char* expected) {
        const CharWritePayloadMirror payload = make_payload(test);
        std::string owner;
        const bool ok = run_owner_write(payload, &owner);
        CHECK(!ok && owner == legacy_character_write_result_json(payload) && owner == expected);
        CHECK(owner.find("\"state\"") == std::string::npos);
    };

    const Case success[] = {
        {QolCharacterWriteKind::kSwitchPlayer, 2, 0, 0, 0},
        {QolCharacterWriteKind::kStopCombat, 0, 0, 0, 0},
        {QolCharacterWriteKind::kStatReset, 0, 0, 0, 0},
        {QolCharacterWriteKind::kSkillReset, 0, 0, 0, 0},
    };

    // 成功体：逐字节对照 legacy；内嵌 kPlayer 子树与 capability 同源；switch 同步 leader_slot。
    for (const auto& test : success) {
        int owner_calls = 0;
        uint8_t owner_leader = 0;
        run_golden(test, &owner_calls, &owner_leader);
        if (test.kind == QolCharacterWriteKind::kSwitchPlayer) {
            // fn_set_active_player 恰一次；成功后 g_main_merc_slot 同步为 slot。
            CHECK(owner_calls == 1 && owner_leader == 2);
        } else {
            CHECK(owner_calls == 0);
        }
    }

    // 写后即时：同一结果处理器读到 live 改动。switch 的改动即 g_main_merc_slot 同步；
    // 其余经主角坐标桩（C_POS_X）改动后被 kPlayer 投影读到。
    g_write_effect_level = 42;
    for (const auto& test : success) {
        const CharWritePayloadMirror payload = make_payload(test);
        sync_legacy_fixture();
        std::string owner;
        CHECK(run_owner_write(payload, &owner));
        if (test.kind == QolCharacterWriteKind::kSwitchPlayer) {
            CHECK(owner.find("\"leader_slot\":2") != std::string::npos);
            CHECK(owner.find("\"x\":3") != std::string::npos);
        } else {
            CHECK(owner.find("\"x\":42") != std::string::npos);
            CHECK(owner.find("\"x\":3") == std::string::npos);
        }
    }
    g_write_effect_level = 0;

    // 非 world：失败体逐字、无 state、票据 result 可回取原错误体。
    g_state_not_in_game = true;
    for (const auto& test : success) {
        run_failure(test, "{\"ok\":false,\"error\":\"not in game\"}");
    }
    g_state_not_in_game = false;

    // switch_player slot 越界：bad slot，且不调原版函数、不动 leader 槽。
    sync_legacy_fixture();
    g_set_active_player_calls = 0;
    run_failure({QolCharacterWriteKind::kSwitchPlayer, 3, 0, 0, 0},
                "{\"ok\":false,\"error\":\"bad slot\"}");
    run_failure({QolCharacterWriteKind::kSwitchPlayer, -1, 0, 0, 0},
                "{\"ok\":false,\"error\":\"bad slot\"}");
    CHECK(g_set_active_player_calls == 0 && g_legacy_leader_slot == 0);

    // switch_player 原版切换失败：switch failed，且先于同步返回、不写 g_main_merc_slot。
    g_set_active_player_result = 0;
    g_set_active_player_calls = 0;
    run_failure({QolCharacterWriteKind::kSwitchPlayer, 1, 0, 0, 0},
                "{\"ok\":false,\"error\":\"switch failed\"}");
    CHECK(g_set_active_player_calls == 2 && g_legacy_leader_slot == 0);
    g_set_active_player_result = 1;

    // role 边界：stop_combat/stat_reset/skill_reset 的 role=9 -> role not found。
    for (const auto kind : {QolCharacterWriteKind::kStopCombat, QolCharacterWriteKind::kStatReset,
                            QolCharacterWriteKind::kSkillReset}) {
        run_failure({kind, 9, 0, 0, 0}, "{\"ok\":false,\"error\":\"role not found\"}");
    }

    // 停攻前置 UI 占用：透传 "ui occupied: <screen>"，且先于角色解析。
    g_ui_blocked_message = "popup";
    run_failure({QolCharacterWriteKind::kStopCombat, 0, 0, 0, 0},
                "{\"ok\":false,\"error\":\"ui occupied: popup\"}");
    g_ui_blocked_message = nullptr;

    // 符号未解析：stat_reset/skill_reset 在初始化函数缺失时 unsupported（先于 member 解析）。
    auto saved_status_init = fn_char_initialize_status;
    fn_char_initialize_status = nullptr;
    run_failure({QolCharacterWriteKind::kStatReset, 0, 0, 0, 0},
                "{\"ok\":false,\"error\":\"symbol not resolved\"}");
    fn_char_initialize_status = saved_status_init;
    auto saved_skill_init = fn_char_initialize_skill;
    fn_char_initialize_skill = nullptr;
    run_failure({QolCharacterWriteKind::kSkillReset, 0, 0, 0, 0},
                "{\"ok\":false,\"error\":\"symbol not resolved\"}");
    fn_char_initialize_skill = saved_skill_init;

    g_state_reads_from_legacy = false;
}

// T2c-4e：skills-state 族（kLearnAction）typed 单路径 + owner 自组装 kSkills 投影覆盖。
// O3：写后 state 必须是 raw 形状（build_skills_json，无技能名注入），与 capability kSkills 同源；
// 写面不得引用 enriched build_character_skills_json（后者仅 GET 读面使用）。
void test_write_skills_family(const qol::contracts::CharacterStateApi* state_api) {
    using qol::contracts::QolCharacterWriteKind;
    g_state_reads_from_legacy = true;
    setup_write_fixture();
    g_write_effect_level = 0;

    struct Case {
        QolCharacterWriteKind kind;
        int32_t i0;
        int32_t i1;
        int32_t i2;
        int64_t i64;
    };
    const auto make_payload = [](const Case& test) {
        CharWritePayloadMirror payload{};
        payload.kind = static_cast<int32_t>(test.kind);
        payload.i0 = test.i0;
        payload.i1 = test.i1;
        payload.i2 = test.i2;
        payload.i64 = test.i64;
        return payload;
    };
    const auto embedded_state = [](const std::string& owner, bool* found) {
        const std::string marker = ",\"state\":";
        const size_t at = owner.find(marker);
        if (at == std::string::npos) {
            *found = false;
            return std::string();
        }
        *found = true;
        return owner.substr(at + marker.size(), owner.size() - at - marker.size() - 1);
    };
    const auto run_failure = [&](const Case& test, const char* expected) {
        const CharWritePayloadMirror payload = make_payload(test);
        std::string owner;
        const bool ok = run_owner_write(payload, &owner);
        CHECK(!ok && owner == legacy_character_write_result_json(payload) && owner == expected);
        CHECK(owner.find("\"state\"") == std::string::npos);
    };

    // 成功体：逐字节对照 legacy；内嵌 kSkills 子树与 capability 同源；O3 raw 形状断言。
    const Case success{QolCharacterWriteKind::kLearnAction, 0, 7, 2, 0};
    {
        const CharWritePayloadMirror payload = make_payload(success);
        sync_legacy_fixture();
        std::string owner;
        const bool ok = run_owner_write(payload, &owner);
        bool found = false;
        const std::string embedded = embedded_state(owner, &found);
        char capability[8192]{};
        uint32_t capability_size = 0;
        const bool capability_ok =
            state_api != nullptr &&
            state_api->write_state_json(qol::contracts::QolCharacterStateKind::kSkills, capability,
                                        sizeof(capability), &capability_size).ok == kQolOk;
        // 同源：owner 内嵌 kSkills == capability kSkills。
        CHECK(found && capability_ok && embedded == std::string(capability, capability_size));
        // O3：raw 形状（无 skill_name），且等于 legacy raw builder 字节。
        CHECK(embedded == build_skills_json());
        CHECK(embedded.find("\"skill_name\"") == std::string::npos);
        CHECK(embedded.find("\"action_id\":7") != std::string::npos);
        sync_legacy_fixture();
        CHECK(ok && owner == legacy_character_write_result_json(payload) &&
              owner.find("\"state\":[") != std::string::npos);
    }

    // 写后即时：原版学习链改 live 技能数据（skill_points 3→1、unlock_bitmap 9→31）后读到新值。
    g_write_effect_level = 1;
    {
        const CharWritePayloadMirror payload = make_payload(success);
        sync_legacy_fixture();
        std::string owner;
        CHECK(run_owner_write(payload, &owner));
        CHECK(owner.find("\"skill_points\":1") != std::string::npos);
        CHECK(owner.find("\"unlock_bitmap\":31") != std::string::npos);
        CHECK(owner.find("\"skill_points\":3") == std::string::npos);
    }
    g_write_effect_level = 0;

    // action_id 传递：legacy 不做 action 合法性校验（不在 owner 层新增 400），任意值透传且结果一致。
    {
        const Case passthrough{QolCharacterWriteKind::kLearnAction, 0, 9999, 3, 0};
        const CharWritePayloadMirror payload = make_payload(passthrough);
        sync_legacy_fixture();
        std::string owner;
        CHECK(run_owner_write(payload, &owner));
        sync_legacy_fixture();
        CHECK(owner == legacy_character_write_result_json(payload));
    }

    // 非 world：失败体逐字、无 state、票据 result 可回取原错误体。
    g_state_not_in_game = true;
    run_failure(success, "{\"ok\":false,\"error\":\"not in game\"}");
    g_state_not_in_game = false;

    // role 边界与符号未解析（结果处理器不新增 400）。
    run_failure({QolCharacterWriteKind::kLearnAction, 9, 7, 2, 0},
                "{\"ok\":false,\"error\":\"role not found\"}");
    auto saved_learn = fn_learn_action;
    fn_learn_action = nullptr;
    run_failure(success, "{\"ok\":false,\"error\":\"symbol not resolved\"}");
    fn_learn_action = saved_learn;

    g_state_reads_from_legacy = false;
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
    CHECK(descriptor.ports[2].id == qol::contracts::QolPortId::QOL_PORT_CHARACTER_WRITE_OPS);
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
    const qol::contracts::QolCharacterStatePort state_port{
        qol::contracts::kQolPortCharacterStateVersion,
        {&fake_state_party, &fake_state_player, &fake_state_skills}};
    const void* ports[] = {&port, &http_port, &kRealWriteOpsPort, &state_port};
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

    // owner 写结果处理器：18 单点 kind 与 legacy builder 逐字节对照；批量与写后即时另断言。
    test_write_golden();
    test_write_no_state_family();
    test_write_party_state_family(state_api);
    test_write_party_member_family(state_api);
    test_write_player_family(state_api);
    test_write_skills_family(state_api);

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
          "{\"ok\":true,\"state\":[{\"role\":0,\"skills\":[{\"action_id\":7,\"level\":2}],"
          "\"unlock_bitmap\":9,\"active_skill_id\":-1,\"skill_points\":3},null,null]}");

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
