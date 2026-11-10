#include "game/ops/character_attributes_read.h"

#include <cstdint>

#include "game/access/char.h"
#include "game/access/game_access.h"
#include "game/adapter_ports/game_host_port.h"
#include "game/state/game_state.h"
#include "game/symbols/game_symbols.h"

namespace qol::game::ops {
namespace {

constexpr int32_t kPartySlotCount = 3;
constexpr int32_t kMercenarySlotCount = 21;

using GetAttrFn = int32_t (*)(void*, int32_t);
using CharacterGetterFn = int32_t (*)(void*);
using HitRateFn = int32_t (*)(void*, void*);
using LoadCharacterExFn = void* (*)(int32_t, int32_t);

GetAttrFn get_attr_fn() {
    static GetAttrFn fn = nullptr;
    if (fn == nullptr && g_base != 0) {
        fn = reinterpret_cast<GetAttrFn>(g_base + fn_resolve("F_GET_ATTR_VMA", F_GET_ATTR_VMA));
    }
    return fn;
}

template <typename Fn>
Fn resolve_getter(const char* symbol, uintptr_t vma) {
    if (g_base == 0) return nullptr;
    return reinterpret_cast<Fn>(g_base + fn_resolve(symbol, vma));
}

int32_t current_save_slot() {
    if (g_base == 0) return -1;
    auto** got = reinterpret_cast<uint8_t**>(g_base + G_CURRENT_SLOT_GOT_VMA);
    if (!game_memory_accessible(got, sizeof(*got), 'r')) return -1;
    uint8_t* slot = *got;
    if (slot == nullptr || !game_memory_accessible(slot, sizeof(*slot), 'r')) return -1;
    return static_cast<int32_t>(*slot);
}

void* load_character_ex(int32_t save_slot, int32_t merc_slot) {
    LoadCharacterExFn fn = resolve_getter<LoadCharacterExFn>(
        "F_SAVE_LOAD_CHARACTER_EX_VMA", F_SAVE_LOAD_CHARACTER_EX_VMA);
    if (fn == nullptr) return nullptr;
    return fn(save_slot, merc_slot);
}

// HP 上限直读 C_MAX_HP 缓存（带可读性校验）；不得调用 CHAR_GetAttr(30)，后者在
// HP > maxHP 时会把 HP 夹回上限并写回角色。
bool read_max_hp_cached(void* character, int32_t* out_value) {
    if (character == nullptr || out_value == nullptr) return false;
    const auto* field = reinterpret_cast<const uint8_t*>(character) + C_MAX_HP;
    if (!game_memory_accessible(field, sizeof(int32_t), 'r')) return false;
    return char_read_max_hp(character, out_value);
}

// MP 上限直读 C_MAX_MP 缓存（带可读性校验）；不得调用 CHAR_GetAttr(31)，后者在
// [ch+0x1f4] > [ch+0xa0] 时会把当前 MP 写回为 maxMP（与 attr30 同构）。
bool read_max_mp_cached(void* character, int32_t* out_value) {
    if (character == nullptr || out_value == nullptr) return false;
    const auto* field = reinterpret_cast<const uint8_t*>(character) + C_MAX_MP;
    if (!game_memory_accessible(field, sizeof(int32_t), 'r')) return false;
    *out_value = *reinterpret_cast<const int32_t*>(field);
    return true;
}

void* resolve_live_character(const contracts::QolCharacterRef& ref) {
    if (ref.kind == contracts::kQolCharacterRefParty) {
        if (ref.index < 0 || ref.index >= kPartySlotCount) return nullptr;
        return char_party_member(ref.index);
    }
    if (ref.kind == contracts::kQolCharacterRefMercenary) {
        if (ref.index < 0 || ref.index >= kMercenarySlotCount) return nullptr;
        return find_char_by_merc_slot(ref.index);
    }
    return nullptr;
}

}  // namespace

bool character_ref_valid(const contracts::QolCharacterRef& ref) {
    if (ref.kind == contracts::kQolCharacterRefParty) {
        return ref.index >= 0 && ref.index < kPartySlotCount;
    }
    if (ref.kind == contracts::kQolCharacterRefMercenary) {
        return ref.index >= 0 && ref.index < kMercenarySlotCount;
    }
    return false;
}

bool character_attributes_present(const contracts::QolCharacterRef& ref) {
    if (!game_host_is_main_thread() || !character_ref_valid(ref)) return false;
    return resolve_live_character(ref) != nullptr;
}

void* resolve_character(const contracts::QolCharacterRef& ref) {
    if (!game_host_is_main_thread() || !character_ref_valid(ref)) return nullptr;
    void* character = resolve_live_character(ref);
    if (character != nullptr) return character;
    // 佣兵槽未命中才按当前存档槽物化；队伍槽不做存档回退。
    if (ref.kind != contracts::kQolCharacterRefMercenary) return nullptr;
    const int32_t save_slot = current_save_slot();
    if (save_slot < 0) return nullptr;
    return load_character_ex(save_slot, ref.index);
}

bool read_character_attr_value(const contracts::QolCharacterRef& ref, int32_t attr_id,
                               int32_t* out_value) {
    if (out_value == nullptr || attr_id < 0 || attr_id >= contracts::kQolCharacterAttrSlotCount) {
        return false;
    }
    if (!game_host_is_main_thread()) return false;
    void* character = resolve_character(ref);
    if (character == nullptr) return false;
    // id30（HP 上限）必须直读 C_MAX_HP 缓存：CHAR_GetAttr(30) 在 HP > maxHP 时会把 HP
    // 夹回上限并写回角色，读属性不应产生副作用。
    if (attr_id == ATTR_MAX_HP) return read_max_hp_cached(character, out_value);
    if (attr_id == ATTR_MAX_MP) return read_max_mp_cached(character, out_value);
    GetAttrFn attr = get_attr_fn();
    if (attr == nullptr) return false;
    *out_value = attr(character, attr_id);
    return true;
}

bool read_character_main_stat_value(const contracts::QolCharacterRef& ref, int32_t index,
                                    int32_t* out_value) {
    if (out_value == nullptr || index < 0 || index >= contracts::kQolCharacterMainStatCount) {
        return false;
    }
    if (!game_host_is_main_thread()) return false;
    void* character = resolve_character(ref);
    if (character == nullptr) return false;
    *out_value = char_stat_total(character, index);
    return true;
}

bool read_character_attribute_snapshot(const contracts::QolCharacterRef& ref,
                                       contracts::QolCharacterAttributeSnapshot* out) {
    if (out == nullptr || !game_host_is_main_thread() || !character_ref_valid(ref)) return false;
    void* character = resolve_character(ref);
    if (character == nullptr) return false;
    GetAttrFn attr = get_attr_fn();

    contracts::QolCharacterAttributeSnapshot snapshot{};
    snapshot.struct_size = sizeof(snapshot);
    snapshot.ref = ref;
    snapshot.present = 1;
    for (int32_t i = 0; i < contracts::kQolCharacterMainStatCount; ++i) {
        snapshot.main_stats[i] = char_stat_total(character, i);
    }
    for (int32_t id = 0; id < contracts::kQolCharacterAttrSlotCount; ++id) {
        // id30（HP 上限）直读 C_MAX_HP 缓存，避免 CHAR_GetAttr(30) 在 HP>maxHP 时钳制写回 HP。
        if (id == ATTR_MAX_HP) {
            const bool max_hp_ok = read_max_hp_cached(character, &snapshot.attrs[id]);
            snapshot.attr_valid[id] = max_hp_ok ? 1 : 0;
            if (!max_hp_ok) snapshot.attrs[id] = 0;
            continue;
        }
        if (id == ATTR_MAX_MP) {
            const bool max_mp_ok = read_max_mp_cached(character, &snapshot.attrs[id]);
            snapshot.attr_valid[id] = max_mp_ok ? 1 : 0;
            if (!max_mp_ok) snapshot.attrs[id] = 0;
            continue;
        }
        if (attr == nullptr) {
            snapshot.attrs[id] = 0;
            snapshot.attr_valid[id] = 0;
            continue;
        }
        snapshot.attrs[id] = attr(character, id);
        snapshot.attr_valid[id] = 1;
    }
    *out = snapshot;
    return true;
}

bool read_character_derived(const contracts::QolCharacterRef& ref, int32_t kind,
                            int32_t* out_value) {
    if (out_value == nullptr || !game_host_is_main_thread()) return false;
    if (kind < contracts::kQolCharacterDerivedNormalDamage ||
        kind > contracts::kQolCharacterDerivedDisplayDamageReduceRate) {
        return false;
    }
    void* character = resolve_character(ref);
    if (character == nullptr) return false;

    switch (kind) {
        case contracts::kQolCharacterDerivedNormalDamage: {
            const CharacterGetterFn fn = resolve_getter<CharacterGetterFn>(
                "F_CHAR_GET_NORMAL_DAMAGE_VMA", F_CHAR_GET_NORMAL_DAMAGE_VMA);
            if (fn == nullptr) return false;
            *out_value = fn(character);
            return true;
        }
        case contracts::kQolCharacterDerivedNormalMagicDamage: {
            const CharacterGetterFn fn = resolve_getter<CharacterGetterFn>(
                "F_CHAR_GET_NORMAL_MAGIC_DAMAGE_VMA", F_CHAR_GET_NORMAL_MAGIC_DAMAGE_VMA);
            if (fn == nullptr) return false;
            *out_value = fn(character);
            return true;
        }
        case contracts::kQolCharacterDerivedPhysicalDefense: {
            // 与原版人物面板一致：attr17 × (attr18 + 1000) / 1000。
            GetAttrFn attr = get_attr_fn();
            if (attr == nullptr) return false;
            const int64_t base = attr(character, 17);
            const int64_t bonus = attr(character, 18);
            *out_value = static_cast<int32_t>(base * (bonus + 1000) / 1000);
            return true;
        }
        case contracts::kQolCharacterDerivedHitRate1000: {
            const HitRateFn fn = resolve_getter<HitRateFn>("F_CHAR_GET_HIT_RATE_1000_VMA",
                                                           F_CHAR_GET_HIT_RATE_1000_VMA);
            if (fn == nullptr) return false;
            *out_value = fn(character, nullptr);
            return true;
        }
        case contracts::kQolCharacterDerivedEvasionRate1000: {
            const CharacterGetterFn fn = resolve_getter<CharacterGetterFn>(
                "F_CHAR_GET_NORMAL_EVASION_RATE_1000_VMA",
                F_CHAR_GET_NORMAL_EVASION_RATE_1000_VMA);
            if (fn == nullptr) return false;
            *out_value = fn(character);
            return true;
        }
        case contracts::kQolCharacterDerivedDisplayDamageReduceRate: {
            const CharacterGetterFn fn = resolve_getter<CharacterGetterFn>(
                "F_CHAR_GET_DISPLAY_DAMAGE_REDUCE_RATE_VMA",
                F_CHAR_GET_DISPLAY_DAMAGE_REDUCE_RATE_VMA);
            if (fn == nullptr) return false;
            *out_value = fn(character);
            return true;
        }
        default:
            return false;
    }
}

}  // namespace qol::game::ops
