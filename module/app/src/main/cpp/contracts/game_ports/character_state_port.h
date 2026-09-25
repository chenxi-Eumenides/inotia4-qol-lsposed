#pragma once

#include <cstdint>
#include <type_traits>

#include "../result.h"
#include "../version.h"

namespace qol::contracts {

constexpr uint32_t kQolCharacterStatePartySize = 3;
constexpr uint32_t kQolCharacterStateEquipSlots = 10;
constexpr uint32_t kQolCharacterStateStatCount = 32;
constexpr uint32_t kQolCharacterStateMainStatCount = 5;
constexpr uint32_t kQolCharacterStateSkillsPerRole = 64;
constexpr uint32_t kQolCharacterStateOptionsPerItem = 32;
constexpr uint32_t kQolCharacterStateNameCapacity = 256;

enum QolCharacterStateMemberField : uint32_t {
    kQolCharacterStateHasBaseStats = 1u << 0,
    kQolCharacterStateHasStatusPoint = 1u << 1,
    kQolCharacterStateHasName = 1u << 2,
};

enum QolCharacterStateItemField : uint32_t {
    kQolCharacterStateItemHasCategory = 1u << 0,
    kQolCharacterStateItemHasCount = 1u << 1,
    kQolCharacterStateItemHasRarity = 1u << 2,
    kQolCharacterStateItemHasDamage = 1u << 3,
    kQolCharacterStateItemHasDefense = 1u << 4,
    kQolCharacterStateItemHasNeedLevel = 1u << 5,
};

struct QolCharacterStateOption {
    int32_t value;
    int32_t id;
    int32_t type;
};

struct QolCharacterStateItem {
    uint32_t fields;
    int32_t type_flags;
    int32_t raw_rarity;
    int32_t category;
    int32_t count;
    int32_t equip;
    int32_t rarity;
    int32_t damage;
    int32_t defense;
    int32_t magic_rate;
    int32_t need_level;
    int32_t socket;
    int32_t socket_filled;
    int32_t socket_total;
    int32_t enchant;
    int32_t chaos;
    int32_t enchant_id;
    int32_t enchant_level;
    int32_t chaos_level;
    int32_t chaos_rate;
    uint32_t option_count;
    QolCharacterStateOption options[kQolCharacterStateOptionsPerItem];
};

struct QolCharacterStateMember {
    uint32_t fields;
    int32_t type;
    int32_t class_idx;
    int32_t name_id;
    int32_t level;
    int32_t hp;
    int32_t mp;
    int32_t max_hp;
    int32_t max_mp;
    int32_t exp;
    int64_t exp_next;
    int32_t stats[kQolCharacterStateStatCount];
    int32_t main_stats[kQolCharacterStateMainStatCount];
    int32_t base_stats[kQolCharacterStateMainStatCount];
    int32_t status_point;
    uint32_t name_size;
    char name[kQolCharacterStateNameCapacity];
    QolCharacterStateItem equipment[kQolCharacterStateEquipSlots];
    uint8_t equipment_present[kQolCharacterStateEquipSlots];
};

struct QolCharacterStateParty {
    QolCharacterStateMember members[kQolCharacterStatePartySize];
    uint8_t member_present[kQolCharacterStatePartySize];
};

struct QolCharacterStatePlayer {
    int64_t money;
    uint32_t map_id;
    int32_t x;
    int32_t y;
    int32_t active_quest;
    int32_t leader_slot;
    int32_t party_count;
};

struct QolCharacterStateSkill {
    int32_t action_id;
    int32_t level;
    int32_t max_level;
    uint8_t has_max_level;
};

struct QolCharacterStateSkillsMember {
    uint32_t skill_count;
    QolCharacterStateSkill skills[kQolCharacterStateSkillsPerRole];
    int32_t unlock_bitmap;
    int32_t active_skill_id;
    int32_t skill_points;
};

struct QolCharacterStateSkills {
    QolCharacterStateSkillsMember members[kQolCharacterStatePartySize];
    uint8_t member_present[kQolCharacterStatePartySize];
};

struct QolCharacterStatePortReadApi {
    QolResult (*party)(QolCharacterStateParty* out_party);
    QolResult (*player)(QolCharacterStatePlayer* out_player);
    QolResult (*skills)(QolCharacterStateSkills* out_skills);
};

struct QolCharacterStatePort {
    QolVersion abi_version;
    QolCharacterStatePortReadApi read;
};

static_assert(std::is_standard_layout<QolCharacterStateOption>::value &&
                  std::is_trivially_copyable<QolCharacterStateOption>::value,
              "QolCharacterStateOption must remain POD");
static_assert(std::is_standard_layout<QolCharacterStateItem>::value &&
                  std::is_trivially_copyable<QolCharacterStateItem>::value,
              "QolCharacterStateItem must remain POD");
static_assert(std::is_standard_layout<QolCharacterStateMember>::value &&
                  std::is_trivially_copyable<QolCharacterStateMember>::value,
              "QolCharacterStateMember must remain POD");
static_assert(std::is_standard_layout<QolCharacterStateParty>::value &&
                  std::is_trivially_copyable<QolCharacterStateParty>::value,
              "QolCharacterStateParty must remain POD");
static_assert(std::is_standard_layout<QolCharacterStatePlayer>::value &&
                  std::is_trivially_copyable<QolCharacterStatePlayer>::value,
              "QolCharacterStatePlayer must remain POD");
static_assert(std::is_standard_layout<QolCharacterStateSkills>::value &&
                  std::is_trivially_copyable<QolCharacterStateSkills>::value,
              "QolCharacterStateSkills must remain POD");
static_assert(std::is_standard_layout<QolCharacterStatePort>::value &&
                  std::is_trivially_copyable<QolCharacterStatePort>::value,
              "QolCharacterStatePort must remain POD");

}  // namespace qol::contracts
