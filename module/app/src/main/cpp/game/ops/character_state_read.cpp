#include "game/ops/character_state_read.h"

#include <cstring>

#include "game/access/game_access.h"
#include "game/read/item_class.h"
#include "game/state/game_state.h"

namespace qol::game::ops {
namespace {

using namespace contracts;

QolResult invalid_argument() {
    return contracts::error(kQolErrInvalidArgument, "invalid character state output", 30);
}

QolResult not_in_game() {
    return contracts::error(kQolErrNotReady, "not in game", 11);
}

QolResult copy_character_name(void* character, QolCharacterStateMember* out) {
    if (fn_get_name == nullptr) return contracts::ok();
    out->fields |= kQolCharacterStateHasName;
    char* name = fn_get_name(character);
    if (name == nullptr) return contracts::ok();
    const size_t size = std::strlen(name);
    if (size >= sizeof(out->name)) {
        return contracts::error(kQolErrBlocked, "character name too long", 23);
    }
    std::memcpy(out->name, name, size);
    out->name_size = static_cast<uint32_t>(size);
    return contracts::ok();
}

QolResult read_item(void* item, QolCharacterStateItem* out) {
    if (item == nullptr || out == nullptr) return invalid_argument();
    *out = {};
    auto* bytes = static_cast<uint8_t*>(item);
    const uint16_t flags = *reinterpret_cast<const uint16_t*>(bytes + I_TYPE);
    out->type_flags = flags;
    out->raw_rarity = (flags >> 2) & 0x0F;
    if (fn_get_bit != nullptr) {
        out->fields |= kQolCharacterStateItemHasCategory;
        out->category = fn_get_bit(flags, 15, 6);
    }
    if (fn_get_cumulate_count != nullptr) {
        out->fields |= kQolCharacterStateItemHasCount;
        out->count = fn_get_cumulate_count(item);
    }
    out->equip = item_is_equip(item) ? 1 : 0;
    if (fn_get_rarity != nullptr) {
        out->fields |= kQolCharacterStateItemHasRarity;
        out->rarity = fn_get_rarity(item);
    }
    if (fn_get_damage != nullptr) {
        out->fields |= kQolCharacterStateItemHasDamage;
        out->damage = fn_get_damage(item);
    }
    if (fn_get_defense != nullptr) {
        out->fields |= kQolCharacterStateItemHasDefense;
        out->defense = fn_get_defense(item);
    }
    out->magic_rate = bytes[I_MAGIC_RATE];
    if (fn_item_get_ability_level != nullptr) {
        out->fields |= kQolCharacterStateItemHasNeedLevel;
        out->need_level = fn_item_get_ability_level(item);
    }
    out->socket = bytes[I_SOCKET];
    out->socket_filled = out->socket & 0x0F;
    out->socket_total = (out->socket >> 4) & 0x0F;
    const uint16_t enchant = *reinterpret_cast<const uint16_t*>(bytes + I_ENCHANT);
    out->enchant = enchant;
    out->chaos = (enchant & 1u) != 0 ? 1 : 0;
    out->enchant_id = (enchant >> 11) & 0x1F;
    out->enchant_level = (enchant >> 6) & 0x1F;
    const uint32_t count = *reinterpret_cast<const uint32_t*>(bytes + I_COUNT);
    out->chaos_level = count & 0xFF;
    out->chaos_rate = (count >> 8) & 0xFF;

    const auto* option = *reinterpret_cast<const uint8_t* const*>(bytes + I_OPTION_LIST);
    while (option != nullptr && out->option_count < kQolCharacterStateOptionsPerItem) {
        const uint16_t index = *reinterpret_cast<const uint16_t*>(option + O_INDEX);
        auto& destination = out->options[out->option_count++];
        destination.value = *reinterpret_cast<const int16_t*>(option + O_VALUE);
        destination.id = index & 0x7F;
        destination.type = (index >> 13) & 0x7;
        option = *reinterpret_cast<const uint8_t* const*>(option + O_NEXT);
    }
    return contracts::ok();
}

QolResult read_member(void* character, QolCharacterStateMember* out) {
    if (character == nullptr || out == nullptr) return invalid_argument();
    *out = {};
    auto* bytes = static_cast<uint8_t*>(character);
    out->type = static_cast<int32_t>(reinterpret_cast<int8_t*>(bytes)[C_TYPE]);
    out->class_idx = static_cast<int32_t>(reinterpret_cast<int8_t*>(bytes)[C_CLASS]);
    out->name_id = *reinterpret_cast<const uint16_t*>(bytes + C_NAME_ID);
    out->level = static_cast<int32_t>(reinterpret_cast<int8_t*>(bytes)[C_LEVEL]);
    out->hp = *reinterpret_cast<const int32_t*>(bytes + C_HP);
    out->mp = *reinterpret_cast<const int32_t*>(bytes + C_MP);
    out->max_hp = char_max_hp(character);
    out->max_mp = char_max_mp(character);
    out->exp = *reinterpret_cast<const int32_t*>(bytes + C_EXP);
    out->exp_next = char_next_exp_cached(character);
    for (uint32_t i = 0; i < kQolCharacterStateStatCount; ++i) {
        out->stats[i] = *reinterpret_cast<const int32_t*>(bytes + C_ATTR + i * sizeof(int32_t));
    }
    for (uint32_t i = 0; i < kQolCharacterStateMainStatCount; ++i) {
        out->main_stats[i] = char_stat_total(character, static_cast<int>(i));
    }
    if (fn_get_stat_base != nullptr) {
        out->fields |= kQolCharacterStateHasBaseStats;
        for (uint32_t i = 0; i < kQolCharacterStateMainStatCount; ++i) {
            out->base_stats[i] = fn_get_stat_base(character, static_cast<int>(i));
        }
    }
    if (fn_get_status_point != nullptr) {
        out->fields |= kQolCharacterStateHasStatusPoint;
        out->status_point = fn_get_status_point(character);
    }
    QolResult result = copy_character_name(character, out);
    if (!contracts::succeeded(result)) return result;

    for (uint32_t slot = 0; slot < kQolCharacterStateEquipSlots; ++slot) {
        void* item = fn_get_equip != nullptr ? fn_get_equip(character, static_cast<int>(slot)) : nullptr;
        if (item == nullptr) continue;
        result = read_item(item, &out->equipment[slot]);
        if (!contracts::succeeded(result)) return result;
        out->equipment_present[slot] = 1;
    }
    return contracts::ok();
}

}  // namespace

QolResult read_character_party(contracts::QolCharacterStateParty* out_party) {
    if (out_party == nullptr) return invalid_argument();
    *out_party = {};
    if (!game_in_world()) return not_in_game();
    for (uint32_t slot = 0; slot < contracts::kQolCharacterStatePartySize; ++slot) {
        void* member = fn_get_member != nullptr ? fn_get_member(static_cast<int>(slot)) : nullptr;
        if (member == nullptr) continue;
        const QolResult result = read_member(member, &out_party->members[slot]);
        if (!contracts::succeeded(result)) return result;
        out_party->member_present[slot] = 1;
    }
    return contracts::ok();
}

QolResult read_character_player(contracts::QolCharacterStatePlayer* out_player) {
    if (out_player == nullptr) return invalid_argument();
    *out_player = {};
    if (!game_in_world()) return not_in_game();
    out_player->money = fn_get_money != nullptr ? fn_get_money() : -1;
    out_player->map_id = current_map_id();
    void* member = lead_member();
    out_player->x = member != nullptr
                        ? *reinterpret_cast<const int16_t*>(static_cast<const uint8_t*>(member) + C_POS_X)
                        : -1;
    out_player->y = member != nullptr
                        ? *reinterpret_cast<const int16_t*>(static_cast<const uint8_t*>(member) + C_POS_Y)
                        : -1;
    out_player->active_quest = g_active_quest != nullptr
                                   ? *reinterpret_cast<const uint16_t*>(g_active_quest)
                                   : -1;
    out_player->leader_slot = g_main_merc_slot != nullptr
                                  ? *reinterpret_cast<const uint8_t*>(g_main_merc_slot)
                                  : -1;
    out_player->party_count = fn_get_party_size != nullptr ? fn_get_party_size() : 3;
    return contracts::ok();
}

QolResult read_character_skills(contracts::QolCharacterStateSkills* out_skills) {
    if (out_skills == nullptr) return invalid_argument();
    *out_skills = {};
    for (uint32_t role = 0; role < contracts::kQolCharacterStatePartySize; ++role) {
        void* member = fn_get_member != nullptr ? fn_get_member(static_cast<int>(role)) : nullptr;
        if (member == nullptr) continue;
        auto& destination = out_skills->members[role];
        auto* bytes = static_cast<uint8_t*>(member);
        auto* node = *reinterpret_cast<uint8_t**>(bytes + C_SKILL_LIST);
        while (node != nullptr && destination.skill_count <
                                      contracts::kQolCharacterStateSkillsPerRole) {
            auto& skill = destination.skills[destination.skill_count++];
            skill.action_id = *reinterpret_cast<const uint16_t*>(node + S_ACTION_ID);
            skill.level = node[S_LEVEL];
            if (fn_get_act_max_level != nullptr) {
                skill.has_max_level = 1;
                skill.max_level = fn_get_act_max_level(member, skill.action_id);
            }
            node = *reinterpret_cast<uint8_t**>(node + S_NEXT);
        }
        destination.unlock_bitmap = *reinterpret_cast<const uint16_t*>(bytes + C_SKILL_BMP);
        auto* active = *reinterpret_cast<uint8_t**>(bytes + C_ACTIVE_SKILL);
        destination.active_skill_id = active != nullptr
                                          ? *reinterpret_cast<const uint16_t*>(active + S_ACTION_ID)
                                          : -1;
        destination.skill_points = static_cast<int32_t>(
            reinterpret_cast<int8_t*>(bytes)[C_SKILL_POINTS]);
        out_skills->member_present[role] = 1;
    }
    return contracts::ok();
}

}  // namespace qol::game::ops
