#include "game/ops/character_write.h"

#include <cstring>
#include <limits>
#include <limits>

#include "contracts/game_cache.h"
#include "game/access/game_access.h"
#include "game/state/game_state.h"

namespace qol::game::ops {
namespace {

// 池槽对象解析：g_base + G_CHAR_POOL_VMA 取角色池首，按 C_OBJ_SIZE 步进并校验对象有效。
void* pool_slot_obj(int slot) {
    if (g_base == 0 || slot < 0 || slot >= C_CHARSYSTEM_POOL_SLOTS) return nullptr;
    uint8_t* pool = *reinterpret_cast<uint8_t**>(g_base + G_CHAR_POOL_VMA);
    if (pool == nullptr) return nullptr;
    uint8_t* obj = pool + slot * C_OBJ_SIZE;
    if (!pool_obj_valid(obj)) return nullptr;
    return obj;
}

QolResult op_fail(contracts::QolCharacterWriteOpResult* out, int32_t code, const char* message) {
    if (out == nullptr || message == nullptr) {
        return contracts::error(kQolErrInvalidArgument, "invalid character write result", 30);
    }
    size_t length = std::strlen(message);
    if (length >= sizeof(out->error_message)) length = sizeof(out->error_message) - 1;
    std::memcpy(out->error_message, message, length);
    out->error_message[length] = '\0';
    out->error_message_len = static_cast<uint32_t>(length);
    return contracts::error(code, out->error_message, out->error_message_len);
}

// 迁移前 op_ok()：成功写操作同步刷新缓存（v0.4.57），恰一次。
QolResult op_ok() {
    frame_cache_force_refresh();
    return contracts::ok();
}

const char* ui_occupied_message(char* message, size_t capacity) {
    const char* screen = ui_blocked();
    if (screen == nullptr) return nullptr;
    constexpr char kPrefix[] = "ui occupied: ";
    const size_t prefix_length = sizeof(kPrefix) - 1;
    if (capacity <= prefix_length) return nullptr;
    std::memcpy(message, kPrefix, prefix_length);
    size_t screen_length = std::strlen(screen);
    const size_t remaining = capacity - prefix_length;
    if (screen_length > remaining) screen_length = remaining;
    std::memcpy(message + prefix_length, screen, screen_length);
    message[prefix_length + screen_length] = '\0';
    return message;
}

}  // namespace

QolResult set_hp(int32_t role, int32_t hp, contracts::QolCharacterWriteOpResult* out) {
    if (out == nullptr) {
        return contracts::error(kQolErrInvalidArgument, "invalid character write result", 30);
    }
    *out = {};
    if (!game_in_world()) return op_fail(out, kQolErrNotReady, "not in game");
    void* ch = member_or_null(role);
    if (ch == nullptr) return op_fail(out, kQolErrNotFound, "role not found");
    if (hp < 0) hp = 0;
    const int32_t max_hp = char_max_hp(ch);
    if (hp > max_hp) hp = max_hp;
    *reinterpret_cast<int32_t*>(reinterpret_cast<uint8_t*>(ch) + C_HP) = hp;
    return op_ok();
}

QolResult set_mp(int32_t role, int32_t mp, contracts::QolCharacterWriteOpResult* out) {
    if (out == nullptr) {
        return contracts::error(kQolErrInvalidArgument, "invalid character write result", 30);
    }
    *out = {};
    if (!game_in_world()) return op_fail(out, kQolErrNotReady, "not in game");
    void* ch = member_or_null(role);
    if (ch == nullptr) return op_fail(out, kQolErrNotFound, "role not found");
    if (mp < 0) mp = 0;
    const int32_t max_mp = char_max_mp(ch);
    if (mp > max_mp) mp = max_mp;
    *reinterpret_cast<int32_t*>(reinterpret_cast<uint8_t*>(ch) + C_MP) = mp;
    return op_ok();
}

QolResult set_level(int32_t role, int32_t level, int32_t force,
                    contracts::QolCharacterWriteOpResult* out) {
    if (out == nullptr) {
        return contracts::error(kQolErrInvalidArgument, "invalid character write result", 30);
    }
    *out = {};
    if (!game_in_world()) return op_fail(out, kQolErrNotReady, "not in game");
    void* ch = member_or_null(role);
    if (ch == nullptr) return op_fail(out, kQolErrNotFound, "role not found");
    if (fn_set_level == nullptr) return op_fail(out, kQolErrUnsupported, "symbol not resolved");
    if (force == 0 && (level < 1 || level > 105)) {
        return op_fail(out, kQolErrInvalidArgument, "level 1-105 (game max)");
    }
    const int ok = fn_set_level(ch, level);
    return ok ? op_ok() : op_fail(out, kQolErrInternal, "level down not allowed");
}

QolResult set_experience(int32_t role, int64_t exp, contracts::QolCharacterWriteOpResult* out) {
    if (out == nullptr) {
        return contracts::error(kQolErrInvalidArgument, "invalid character write result", 30);
    }
    *out = {};
    if (!game_in_world()) return op_fail(out, kQolErrNotReady, "not in game");
    if (exp < std::numeric_limits<int32_t>::min() ||
        exp > std::numeric_limits<int32_t>::max()) {
        return op_fail(out, kQolErrInvalidArgument, "exp required");
    }
    void* ch = member_or_null(role);
    if (ch == nullptr) return op_fail(out, kQolErrNotFound, "role not found");
    if (fn_set_exp == nullptr) return op_fail(out, kQolErrUnsupported, "symbol not resolved");
    fn_set_exp(ch, static_cast<int32_t>(exp));
    return op_ok();
}

QolResult set_attr(int32_t role, int32_t attr_index, int32_t value,
                   contracts::QolCharacterWriteOpResult* out) {
    if (out == nullptr) {
        return contracts::error(kQolErrInvalidArgument, "invalid character write result", 30);
    }
    *out = {};
    if (!game_in_world()) return op_fail(out, kQolErrNotReady, "not in game");
    if (attr_index < 0 || attr_index > 4) return op_fail(out, kQolErrInvalidArgument, "bad attr");
    if (fn_set_stat_base == nullptr) return op_fail(out, kQolErrUnsupported, "symbol not resolved");
    void* ch = member_or_null(role);
    if (ch == nullptr) return op_fail(out, kQolErrNotFound, "role not found");
    if (value < 0) value = 0;
    if (value > 255) value = 255;
    fn_set_stat_base(ch, attr_index, value);
    return op_ok();
}

QolResult set_status_point(int32_t role, int32_t points,
                           contracts::QolCharacterWriteOpResult* out) {
    if (out == nullptr) {
        return contracts::error(kQolErrInvalidArgument, "invalid character write result", 30);
    }
    *out = {};
    if (!game_in_world()) return op_fail(out, kQolErrNotReady, "not in game");
    void* ch = member_or_null(role);
    if (ch == nullptr) return op_fail(out, kQolErrNotFound, "role not found");
    if (fn_set_status_point == nullptr) {
        return op_fail(out, kQolErrUnsupported, "symbol not resolved");
    }
    fn_set_status_point(ch, points);
    return op_ok();
}

QolResult party_swap(int32_t a, int32_t b, contracts::QolCharacterWriteOpResult* out) {
    if (out == nullptr) {
        return contracts::error(kQolErrInvalidArgument, "invalid character write result", 30);
    }
    *out = {};
    if (!game_in_world()) return op_fail(out, kQolErrNotReady, "not in game");
    if (fn_party_swap == nullptr) return op_fail(out, kQolErrUnsupported, "symbol not resolved");
    if (a < 0 || a > 2 || b < 0 || b > 2) return op_fail(out, kQolErrInvalidArgument, "bad slot");
    fn_party_swap(a, b);
    return op_ok();
}

QolResult set_auto_attack(int32_t role, int32_t onoff,
                          contracts::QolCharacterWriteOpResult* out) {
    if (out == nullptr) {
        return contracts::error(kQolErrInvalidArgument, "invalid character write result", 30);
    }
    *out = {};
    if (!game_in_world()) return op_fail(out, kQolErrNotReady, "not in game");
    void* ch = member_or_null(role);
    if (ch == nullptr) return op_fail(out, kQolErrNotFound, "role not found");
    if (fn_set_auto_attack == nullptr) {
        return op_fail(out, kQolErrUnsupported, "symbol not resolved");
    }
    fn_set_auto_attack(ch, onoff != 0 ? 1 : 0);
    return op_ok();
}

QolResult cast_skill(int32_t role, int32_t action_id, contracts::QolCharacterWriteOpResult* out) {
    if (out == nullptr) {
        return contracts::error(kQolErrInvalidArgument, "invalid character write result", 30);
    }
    *out = {};
    if (!game_in_world()) return op_fail(out, kQolErrNotReady, "not in game");
    char message[sizeof(out->error_message)]{};
    if (ui_occupied_message(message, sizeof(message)) != nullptr) {
        return op_fail(out, kQolErrBlocked, message);
    }
    void* ch = member_or_null(role);
    if (ch == nullptr) return op_fail(out, kQolErrNotFound, "role not found");
    if (fn_char_get_enemy_target == nullptr || fn_char_set_action_id == nullptr) {
        return op_fail(out, kQolErrUnsupported, "symbol not resolved");
    }
    uint8_t* node = *reinterpret_cast<uint8_t**>(reinterpret_cast<uint8_t*>(ch) + C_SKILL_LIST);
    bool learned = false;
    int count = 0;
    while (node != nullptr && count < 64) {
        if (*reinterpret_cast<uint16_t*>(node + S_ACTION_ID) ==
            static_cast<uint16_t>(action_id)) {
            learned = true;
            break;
        }
        node = *reinterpret_cast<uint8_t**>(node + S_NEXT);
        ++count;
    }
    if (!learned) return op_fail(out, kQolErrBlocked, "skill not learned");
    void* target = fn_char_get_enemy_target(ch, 0, 0);
    if (target == nullptr) return op_fail(out, kQolErrBlocked, "no target");
    fn_char_set_action_id(ch, action_id, target);
    return op_ok();
}

QolResult attack_target(int32_t role, int32_t target_slot,
                        contracts::QolCharacterWriteOpResult* out) {
    if (out == nullptr) {
        return contracts::error(kQolErrInvalidArgument, "invalid character write result", 30);
    }
    *out = {};
    if (!game_in_world()) return op_fail(out, kQolErrNotReady, "not in game");
    char message[sizeof(out->error_message)]{};
    if (ui_occupied_message(message, sizeof(message)) != nullptr) {
        return op_fail(out, kQolErrBlocked, message);
    }
    if (fn_char_set_target == nullptr || fn_char_set_action_id == nullptr) {
        return op_fail(out, kQolErrUnsupported, "symbol not resolved");
    }
    void* ch = member_or_null(role);
    if (ch == nullptr) return op_fail(out, kQolErrNotFound, "role not found");
    void* target = pool_slot_obj(target_slot);
    if (target == nullptr) return op_fail(out, kQolErrNotFound, "target not found");
    fn_char_set_target(ch, target);
    fn_char_set_action_id(ch, 5, target);
    return op_ok();
}

QolResult switch_player(int32_t slot, contracts::QolCharacterWriteOpResult* out) {
    if (out == nullptr) {
        return contracts::error(kQolErrInvalidArgument, "invalid character write result", 30);
    }
    *out = {};
    if (!game_in_world()) return op_fail(out, kQolErrNotReady, "not in game");
    if (fn_set_active_player == nullptr) {
        return op_fail(out, kQolErrUnsupported, "symbol not resolved");
    }
    if (slot < 0 || slot > 2) return op_fail(out, kQolErrInvalidArgument, "bad slot");
    const int r = fn_set_active_player(slot);
    if (!r) return op_fail(out, kQolErrInternal, "switch failed");
    // PARTY_SetActivePlayer 只写 PLAYER_pActivePlayer，不同步 SAVE_nMainMercenarySlot。
    if (g_main_merc_slot != nullptr) {
        *reinterpret_cast<uint8_t*>(g_main_merc_slot) = static_cast<uint8_t>(slot);
    }
    return op_ok();
}

QolResult stop_combat(int32_t role, contracts::QolCharacterWriteOpResult* out) {
    if (out == nullptr) {
        return contracts::error(kQolErrInvalidArgument, "invalid character write result", 30);
    }
    *out = {};
    if (!game_in_world()) return op_fail(out, kQolErrNotReady, "not in game");
    char message[sizeof(out->error_message)]{};
    if (ui_occupied_message(message, sizeof(message)) != nullptr) {
        return op_fail(out, kQolErrBlocked, message);
    }
    if (fn_char_stop_combat == nullptr) {
        return op_fail(out, kQolErrUnsupported, "symbol not resolved");
    }
    void* ch = member_or_null(role);
    if (ch == nullptr) return op_fail(out, kQolErrNotFound, "role not found");
    fn_char_stop_combat(ch);
    return op_ok();
}

QolResult learn_action(int32_t role, int32_t action_id, int32_t level,
                       contracts::QolCharacterWriteOpResult* out) {
    if (out == nullptr) {
        return contracts::error(kQolErrInvalidArgument, "invalid character write result", 30);
    }
    *out = {};
    if (!game_in_world()) return op_fail(out, kQolErrNotReady, "not in game");
    void* ch = member_or_null(role);
    if (ch == nullptr) return op_fail(out, kQolErrNotFound, "role not found");
    if (fn_learn_action == nullptr) return op_fail(out, kQolErrUnsupported, "symbol not resolved");
    fn_learn_action(ch, action_id, level);
    return op_ok();
}

QolResult add_stat(int32_t role, int32_t attr_index, contracts::QolCharacterWriteOpResult* out) {
    if (out == nullptr) {
        return contracts::error(kQolErrInvalidArgument, "invalid character write result", 30);
    }
    *out = {};
    if (!game_in_world()) return op_fail(out, kQolErrNotReady, "not in game");
    void* ch = member_or_null(role);
    if (ch == nullptr) return op_fail(out, kQolErrNotFound, "role not found");
    if (fn_get_status_point == nullptr || fn_get_stat_main == nullptr ||
        fn_set_stat_main == nullptr || fn_set_status_point == nullptr) {
        return op_fail(out, kQolErrUnsupported, "symbol not resolved");
    }
    if (attr_index < 0 || attr_index > 4) return op_fail(out, kQolErrInvalidArgument, "bad attr");
    // 原版属性分配：读剩余能力点 → 判 >0 → 属性 +1 → 能力点 -1，单请求内同步执行。
    const int32_t points = fn_get_status_point(ch);
    if (points <= 0) return op_fail(out, kQolErrBlocked, "no status point");
    const int32_t current = fn_get_stat_main(ch, attr_index);
    fn_set_stat_main(ch, attr_index, current + 1);
    fn_set_status_point(ch, points - 1);
    return op_ok();
}

QolResult stat_reset(int32_t role, contracts::QolCharacterWriteOpResult* out) {
    if (out == nullptr) {
        return contracts::error(kQolErrInvalidArgument, "invalid character write result", 30);
    }
    *out = {};
    if (!game_in_world()) return op_fail(out, kQolErrNotReady, "not in game");
    void* ch = member_or_null(role);
    if (ch == nullptr) return op_fail(out, kQolErrNotFound, "role not found");
    if (fn_char_initialize_status == nullptr) {
        return op_fail(out, kQolErrUnsupported, "symbol not resolved");
    }
    fn_char_initialize_status(ch);
    return op_ok();
}

QolResult skill_reset(int32_t role, contracts::QolCharacterWriteOpResult* out) {
    if (out == nullptr) {
        return contracts::error(kQolErrInvalidArgument, "invalid character write result", 30);
    }
    *out = {};
    if (!game_in_world()) return op_fail(out, kQolErrNotReady, "not in game");
    void* ch = member_or_null(role);
    if (ch == nullptr) return op_fail(out, kQolErrNotFound, "role not found");
    if (fn_char_initialize_skill == nullptr) {
        return op_fail(out, kQolErrUnsupported, "symbol not resolved");
    }
    fn_char_initialize_skill(ch);
    return op_ok();
}

QolResult party_include(int32_t mercenary_slot, contracts::QolCharacterWriteOpResult* out) {
    if (out == nullptr) {
        return contracts::error(kQolErrInvalidArgument, "invalid character write result", 30);
    }
    *out = {};
    if (!game_in_world()) return op_fail(out, kQolErrNotReady, "not in game");
    if (fn_include_party == nullptr || fn_get_party_size == nullptr || fn_get_member == nullptr) {
        return op_fail(out, kQolErrUnsupported, "symbol not resolved");
    }
    void* ch = find_char_by_merc_slot(mercenary_slot);
    if (ch == nullptr) return op_fail(out, kQolErrNotFound, "mercenary not found");
    for (int i = 0; i < 3; ++i) {
        if (fn_get_member(i) == ch) return op_fail(out, kQolErrConflict, "already in party");
    }
    if (fn_get_party_size() >= 3) return op_fail(out, kQolErrBlocked, "party full");
    const int r = fn_include_party(ch);
    return r ? op_ok() : op_fail(out, kQolErrInternal, "party full or include failed");
}

QolResult party_exclude(int32_t mercenary_slot, contracts::QolCharacterWriteOpResult* out) {
    if (out == nullptr) {
        return contracts::error(kQolErrInvalidArgument, "invalid character write result", 30);
    }
    *out = {};
    if (!game_in_world()) return op_fail(out, kQolErrNotReady, "not in game");
    if (fn_exclude_party == nullptr || fn_get_member == nullptr) {
        return op_fail(out, kQolErrUnsupported, "symbol not resolved");
    }
    void* ch = find_char_by_merc_slot(mercenary_slot);
    if (ch == nullptr) return op_fail(out, kQolErrNotFound, "mercenary not found");
    if (fn_get_member(0) == ch) return op_fail(out, kQolErrBlocked, "cannot exclude leader");
    if (fn_is_special_npc != nullptr && fn_is_special_npc(ch)) {
        return op_fail(out, kQolErrBlocked, "cannot exclude quest npc");
    }
    const int r = fn_exclude_party(ch);
    return r ? op_ok() : op_fail(out, kQolErrInternal, "exclude failed");
}

QolResult party_discharge(int32_t mercenary_slot, contracts::QolCharacterWriteOpResult* out) {
    if (out == nullptr) {
        return contracts::error(kQolErrInvalidArgument, "invalid character write result", 30);
    }
    *out = {};
    if (!game_in_world()) return op_fail(out, kQolErrNotReady, "not in game");
    if (fn_mercenary_release == nullptr || fn_get_member == nullptr) {
        return op_fail(out, kQolErrUnsupported, "symbol not resolved");
    }
    void* ch = find_char_by_merc_slot(mercenary_slot);
    if (ch == nullptr) return op_fail(out, kQolErrNotFound, "mercenary not found");
    if (fn_get_member(0) == ch) return op_fail(out, kQolErrBlocked, "cannot discharge leader");
    if (fn_is_special_npc != nullptr && fn_is_special_npc(ch)) {
        return op_fail(out, kQolErrBlocked, "cannot discharge quest npc");
    }
    // 原版 MERCENARYSYSTEM_Release 内部完整链承接遣散副作用。
    fn_mercenary_release(mercenary_slot);
    return op_ok();
}

QolResult party_withdraw(int32_t mercenary_slot, int32_t equip_slot,
                         contracts::QolCharacterWriteOpResult* out) {
    if (out == nullptr) {
        return contracts::error(kQolErrInvalidArgument, "invalid character write result", 30);
    }
    *out = {};
    if (!game_in_world()) return op_fail(out, kQolErrNotReady, "not in game");
    if (fn_unequip == nullptr) return op_fail(out, kQolErrUnsupported, "symbol not resolved");
    void* ch = find_char_by_merc_slot(mercenary_slot);
    if (ch == nullptr) return op_fail(out, kQolErrNotFound, "mercenary not found");
    if (equip_slot < 0 || equip_slot >= C_EQUIP_SLOTS) {
        return op_fail(out, kQolErrInvalidArgument, "bad slot");
    }
    // 与原版脱装备按钮链同口径：先经 CHAR_CanUnequipItem（不可脱标记 + 特殊 NPC 闸门）。
    if (fn_can_unequip != nullptr) {
        void* item = *reinterpret_cast<void**>(reinterpret_cast<uint8_t*>(ch) + C_EQUIP +
                                               equip_slot * 8);
        if (item != nullptr && fn_can_unequip(ch, item) == 0) {
            return op_fail(out, kQolErrBlocked, "cannot unequip");
        }
    }
    const int r = fn_unequip(ch, equip_slot);
    return r ? op_ok() : op_fail(out, kQolErrInternal, "withdraw failed");
}

QolResult add_experience(int32_t role, int64_t delta, contracts::QolCharacterWriteOpResult* out) {
    if (out == nullptr) {
        return contracts::error(kQolErrInvalidArgument, "invalid character write result", 30);
    }
    *out = {};
    if (!game_in_world()) return op_fail(out, kQolErrNotReady, "not in game");
    void* ch = member_or_null(role);
    if (ch == nullptr) return op_fail(out, kQolErrNotFound, "role not found");
    if (fn_add_exp == nullptr) return op_fail(out, kQolErrUnsupported, "symbol not resolved");
    const int r = fn_add_exp(ch, static_cast<int32_t>(delta), 1);
    return r ? op_ok() : op_fail(out, kQolErrInternal, "add exp failed");
}

QolResult set_skill_usage(int32_t role, int32_t onoff, contracts::QolCharacterWriteOpResult* out) {
    if (out == nullptr) {
        return contracts::error(kQolErrInvalidArgument, "invalid character write result", 30);
    }
    *out = {};
    if (!game_in_world()) return op_fail(out, kQolErrNotReady, "not in game");
    void* ch = member_or_null(role);
    if (ch == nullptr) return op_fail(out, kQolErrNotFound, "role not found");
    if (fn_set_skill_usage == nullptr) {
        return op_fail(out, kQolErrUnsupported, "symbol not resolved");
    }
    fn_set_skill_usage(ch, onoff ? 1 : 0);
    return op_ok();
}

}  // namespace qol::game::ops
