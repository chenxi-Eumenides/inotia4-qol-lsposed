// game_character.cpp —— 角色/战斗域：战斗操作 + 角色数值写操作（parse 域）
// 由 game_ops_action.cpp / game_ops_value.cpp 拆分生成（纯搬代码，零逻辑变更）。

#include "game_character.h"

#include "game_access.h"
#include "game_state.h"
#include "game_ops_common.h"
#include "game_inventory.h"
#include "game_json.h"
#include "game_world.h"

#include <cstdint>
#include <string>

void* pool_slot_obj(int slot) {
    if (g_base == 0 || slot < 0 || slot >= C_CHARSYSTEM_POOL_SLOTS) return nullptr;
    uint8_t* pool = *reinterpret_cast<uint8_t**>(g_base + G_CHAR_POOL_VMA);
    if (pool == nullptr) return nullptr;
    uint8_t* obj = pool + slot * C_OBJ_SIZE;
    if (!pool_obj_valid(obj)) return nullptr;
    return obj;
}

std::string data_op_stat_reset(int role) {
    if (!game_in_world()) return op_err("not in game");
    void* ch = member_or_null(role);
    if (ch == nullptr) return op_err("role not found");
    if (fn_char_initialize_status == nullptr) return op_err("symbol not resolved");
    fn_char_initialize_status(ch);
    return op_ok();
}
std::string data_op_skill_reset(int role) {
    if (!game_in_world()) return op_err("not in game");
    void* ch = member_or_null(role);
    if (ch == nullptr) return op_err("role not found");
    if (fn_char_initialize_skill == nullptr) return op_err("symbol not resolved");
    fn_char_initialize_skill(ch);
    return op_ok();
}
std::string data_op_cast(int role, int32_t action_id) {
    if (!game_in_world()) return op_err("not in game");
    if (const char* ui = ui_blocked()) {
        std::string err = "ui occupied: ";
        err += ui;
        return op_err(err.c_str());
    }
    void* ch = member_or_null(role);
    if (ch == nullptr) return op_err("role not found");
    if (fn_char_get_enemy_target == nullptr || fn_char_set_action_id == nullptr)
        return op_err("symbol not resolved");
    // 校验技能已学（遍历 +0x2A0 技能链表）
    uint8_t* node = *reinterpret_cast<uint8_t**>(reinterpret_cast<uint8_t*>(ch) + C_SKILL_LIST);
    bool learned = false;
    int count = 0;
    while (node != nullptr && count < 64) {
        if (*reinterpret_cast<uint16_t*>(node + S_ACTION_ID) == static_cast<uint16_t>(action_id)) {
            learned = true;
            break;
        }
        node = *reinterpret_cast<uint8_t**>(node + S_NEXT);
        ++count;
    }
    if (!learned) return op_err("skill not learned");
    // 获取合法敌人目标（无目标不释放）
    void* target = fn_char_get_enemy_target(ch, 0, 0);
    if (target == nullptr) return op_err("no target");
    fn_char_set_action_id(ch, action_id, target);
    return op_ok();
}
std::string data_op_attack(int role, int target_slot) {
    if (!game_in_world()) return op_err("not in game");
    if (const char* ui = ui_blocked()) {
        std::string err = "ui occupied: ";
        err += ui;
        return op_err(err.c_str());
    }
    if (fn_char_set_target == nullptr || fn_char_set_action_id == nullptr)
        return op_err("symbol not resolved");
    void* ch = member_or_null(role);
    if (ch == nullptr) return op_err("role not found");
    void* target = pool_slot_obj(target_slot);
    if (target == nullptr) return op_err("target not found");
    fn_char_set_target(ch, target);
    fn_char_set_action_id(ch, 5, target);
    return op_ok();
}
std::string data_op_stop_combat(int role) {
    if (!game_in_world()) return op_err("not in game");
    if (const char* ui = ui_blocked()) {
        std::string err = "ui occupied: ";
        err += ui;
        return op_err(err.c_str());
    }
    if (fn_char_stop_combat == nullptr) return op_err("symbol not resolved");
    void* ch = member_or_null(role);
    if (ch == nullptr) return op_err("role not found");
    fn_char_stop_combat(ch);
    return op_ok();
}

std::string data_op_set_experience(int role, int64_t exp) {
    if (!game_in_world()) return op_err("not in game");
    void* ch = member_or_null(role);
    if (ch == nullptr) return op_err("role not found");
    if (fn_set_exp == nullptr) return op_err("symbol not resolved");
    fn_set_exp(ch, static_cast<int32_t>(exp));
    return op_ok();
}

std::string data_op_set_level(int role, int32_t level, bool force) {
    if (!game_in_world()) return op_err("not in game");
    void* ch = member_or_null(role);
    if (ch == nullptr) return op_err("role not found");
    if (fn_set_level == nullptr) return op_err("symbol not resolved");
    // 默认限制游戏实际上限 105（EXP 表 105 级）；force=true 跳过校验（level 超出 s8 存储 127 会溢出为负，调用方自担）
    if (!force && (level < 1 || level > 105)) return op_err("level 1-105 (game max)");
    // CHAR_SetLevel 完整结算：写 [ch+0xe] + 重算 nextExp + InitializeFromLevel + 升级加能力点/技能点 + 回满血蓝
    int ok = fn_set_level(ch, level);
    return ok ? op_ok() : op_err("level down not allowed");
}

std::string data_op_add_experience(int role, int64_t delta) {
    if (!game_in_world()) return op_err("not in game");
    void* ch = member_or_null(role);
    if (ch == nullptr) return op_err("role not found");
    if (fn_add_exp == nullptr) return op_err("symbol not resolved");
    int r = fn_add_exp(ch, static_cast<int32_t>(delta), 1);
    return r ? op_ok() : op_err("add exp failed");
}

std::string data_op_set_status_point(int role, int32_t points) {
    if (!game_in_world()) return op_err("not in game");
    void* ch = member_or_null(role);
    if (ch == nullptr) return op_err("role not found");
    if (fn_set_status_point == nullptr) return op_err("symbol not resolved");
    fn_set_status_point(ch, points);
    return op_ok();
}

std::string data_op_add_stat(int role, int32_t attr) {
    if (!game_in_world()) return op_err("not in game");
    void* ch = member_or_null(role);
    if (ch == nullptr) return op_err("role not found");
    if (fn_get_status_point == nullptr || fn_get_stat_main == nullptr || fn_set_stat_main == nullptr ||
        fn_set_status_point == nullptr)
        return op_err("symbol not resolved");
    if (attr < 0 || attr > 4) return op_err("bad attr");
    int32_t points = fn_get_status_point(ch);
    if (points <= 0) return op_err("no status point");
    int32_t cur = fn_get_stat_main(ch, attr);
    fn_set_stat_main(ch, attr, cur + 1);
    fn_set_status_point(ch, points - 1);
    return op_ok();
}

std::string data_op_set_auto_attack(int role, int32_t onoff) {
    if (!game_in_world()) return op_err("not in game");
    void* ch = member_or_null(role);
    if (ch == nullptr) return op_err("role not found");
    if (fn_set_auto_attack == nullptr) return op_err("symbol not resolved");
    fn_set_auto_attack(ch, onoff ? 1 : 0);
    return op_ok();
}

std::string data_op_set_skill_usage(int role, int32_t onoff) {
    if (!game_in_world()) return op_err("not in game");
    void* ch = member_or_null(role);
    if (ch == nullptr) return op_err("role not found");
    if (fn_set_skill_usage == nullptr) return op_err("symbol not resolved");
    fn_set_skill_usage(ch, onoff ? 1 : 0);
    return op_ok();
}

std::string data_op_learn_action(int role, int32_t action_id, int32_t level) {
    if (!game_in_world()) return op_err("not in game");
    void* ch = member_or_null(role);
    if (ch == nullptr) return op_err("role not found");
    if (fn_learn_action == nullptr) return op_err("symbol not resolved");
    fn_learn_action(ch, action_id, level);
    return op_ok();
}

std::string data_op_set_hp(int role, int32_t hp) {
    if (!game_in_world()) return op_err("not in game");
    void* ch = member_or_null(role);
    if (ch == nullptr) return op_err("role not found");
    if (hp < 0) hp = 0;
    int32_t max_hp = fn_get_attr != nullptr ? fn_get_attr(ch, 0x1e) : 32767;
    if (hp > max_hp) hp = max_hp;
    *reinterpret_cast<int32_t*>(reinterpret_cast<uint8_t*>(ch) + C_HP) = hp;
    return op_ok();
}

std::string data_op_set_mp(int role, int32_t mp) {
    if (!game_in_world()) return op_err("not in game");
    void* ch = member_or_null(role);
    if (ch == nullptr) return op_err("role not found");
    if (mp < 0) mp = 0;
    int32_t max_mp = fn_get_attr != nullptr ? fn_get_attr(ch, 0x1f) : 32767;
    if (mp > max_mp) mp = max_mp;
    *reinterpret_cast<int32_t*>(reinterpret_cast<uint8_t*>(ch) + C_MP) = mp;
    return op_ok();
}

std::string data_op_set_attr(int role, int attr_index, int32_t value) {
    if (!game_in_world()) return op_err("not in game");
    if (attr_index < 0 || attr_index > 4) return op_err("bad attr");
    if (fn_set_stat_base == nullptr) return op_err("symbol not resolved");
    void* ch = member_or_null(role);
    if (ch == nullptr) return op_err("role not found");
    if (value < 0) value = 0;
    if (value > 255) value = 255;
    // CHAR_SetStatBase 直接写"基础属性"（[ch+0x250+i] s8），装备/分配/动态加成不受影响
    // 总属性 = 基础 + 分配(main) + 加成(bonus) + 动态(equip/skill/buff)
    fn_set_stat_base(ch, attr_index, value);
    return op_ok();
}

std::string member_json(void* ch) {
    std::string s = "{";
    s += "\"type\":" + std::to_string(static_cast<int>(reinterpret_cast<int8_t*>(ch)[C_TYPE]));
    s += ",\"class_idx\":" + std::to_string(static_cast<int>(reinterpret_cast<int8_t*>(ch)[C_CLASS]));
    uint16_t name_id = *reinterpret_cast<uint16_t*>(reinterpret_cast<uint8_t*>(ch) + C_NAME_ID);
    s += ",\"name_id\":" + std::to_string(name_id);
    s += ",\"level\":" + std::to_string(static_cast<int>(reinterpret_cast<int8_t*>(ch)[C_LEVEL]));
    s += ",\"hp\":" + std::to_string(*reinterpret_cast<int32_t*>(reinterpret_cast<uint8_t*>(ch) + C_HP));
    s += ",\"mp\":" + std::to_string(*reinterpret_cast<int32_t*>(reinterpret_cast<uint8_t*>(ch) + C_MP));
    if (fn_get_attr != nullptr) {
        s += ",\"max_hp\":" + std::to_string(fn_get_attr(ch, ATTR_MAX_HP));
        s += ",\"max_mp\":" + std::to_string(fn_get_attr(ch, ATTR_MAX_MP));
    }
    if (fn_get_exp != nullptr) {
        s += ",\"exp\":" + std::to_string(fn_get_exp(ch));
        if (fn_get_next_exp != nullptr) {
            s += ",\"exp_next\":" + std::to_string(fn_get_next_exp(ch));
        }
    }
    s += ",\"stats\":{";
    bool first = true;
    for (int a = 0; a < 32; ++a) {
        int32_t v = *reinterpret_cast<int32_t*>(reinterpret_cast<uint8_t*>(ch) + C_ATTR + a * 4);
        if (!first) s += ",";
        s += "\"" + std::to_string(a) + "\":" + std::to_string(v);
        first = false;
    }
    s += "}";
    if (fn_get_stat != nullptr) {
        s += ",\"main_stats\":[";
        for (int a = 0; a < 5; ++a) {
            if (a > 0) s += ",";
            s += std::to_string(fn_get_stat(ch, a));
        }
        s += "]";
    }
    if (fn_get_stat_base != nullptr) {
        s += ",\"base_stats\":[";
        for (int a = 0; a < 5; ++a) {
            if (a > 0) s += ",";
            s += std::to_string(fn_get_stat_base(ch, a));
        }
        s += "]";
    }
    if (fn_get_status_point != nullptr) {
        s += ",\"status_point\":" + std::to_string(fn_get_status_point(ch));
    }
    s += ",\"equipment\":[";
    for (int slot = 0; slot < C_EQUIP_SLOTS; ++slot) {
        void* item = nullptr;
        if (fn_get_equip != nullptr) item = fn_get_equip(ch, slot);
        if (slot > 0) s += ",";
        if (item == nullptr) {
            s += "null";
        } else {
            uint16_t flags = *reinterpret_cast<uint16_t*>(reinterpret_cast<uint8_t*>(item) + I_TYPE);
            s += "{\"slot\":" + std::to_string(slot);
            s += ",\"type_flags\":" + std::to_string(flags);
            s += ",\"raw_rarity\":" + std::to_string((flags >> 2) & 0x0F);
            if (fn_get_bit != nullptr) {
                s += ",\"category\":" + std::to_string(fn_get_bit(flags, 15, 6));
            }
            if (fn_get_cumulate_count != nullptr) {
                s += ",\"count\":" + std::to_string(fn_get_cumulate_count(item));
            }
            s += std::string(",\"equip\":") + (item_is_equip(item) ? "true" : "false");
            if (fn_get_rarity != nullptr) {
                s += ",\"rarity\":" + std::to_string(fn_get_rarity(item));
            }
            append_item_attrs(s, item);
            s += "}";
        }
    }
    s += "]";
    if (fn_get_name != nullptr) {
        s += ",\"name\":\"" + json_escape(fn_get_name(ch)) + "\"";
    }
    s += "}";
    return s;
}

std::string build_player_json() {
    if (!game_in_world()) return "{\"error\":\"not in game\"}";
    std::string s = "{";
    s += "\"money\":" + std::to_string(fn_get_money != nullptr ? fn_get_money() : -1);
    s += ",\"map_id\":" + std::to_string(current_map_id());
    append_position(s, lead_member());
    s += ",\"active_quest\":" + std::to_string(g_active_quest != nullptr ? *reinterpret_cast<uint16_t*>(g_active_quest) : -1);
    s += ",\"leader_slot\":" + std::to_string(g_main_merc_slot != nullptr ? *reinterpret_cast<uint8_t*>(g_main_merc_slot) : -1);
    s += ",\"party_count\":" + std::to_string(fn_get_party_size != nullptr ? fn_get_party_size() : 3);
    s += "}";
    return s;
}

std::string build_skills_json() {
    // 每角色技能：+0x2A0 链表（节点 action_id/level/next）、+0x2B0 解锁位图、
    // +0x280 当前技能、+0x328 剩余技能点。链表节点步长由 next(+0x18) 驱动。
    std::string s = "[";
    for (int i = 0; i < 3; ++i) {
        void* ch = (fn_get_member != nullptr) ? fn_get_member(i) : nullptr;
        if (i > 0) s += ",";
        if (ch == nullptr) {
            s += "null";
            continue;
        }
        uint8_t* base_ch = reinterpret_cast<uint8_t*>(ch);
        s += "{\"role\":" + std::to_string(i);
        s += ",\"skills\":[";
        uint8_t* node = *reinterpret_cast<uint8_t**>(base_ch + C_SKILL_LIST);
        bool first = true;
        int count = 0;
        while (node != nullptr && count < 64) {
            if (!first) s += ",";
            s += "{\"action_id\":" + std::to_string(*reinterpret_cast<uint16_t*>(node + S_ACTION_ID));
            s += ",\"level\":" + std::to_string(node[S_LEVEL]);
            if (fn_get_act_max_level != nullptr) {
                s += ",\"max_level\":" + std::to_string(fn_get_act_max_level(ch, *reinterpret_cast<uint16_t*>(node + S_ACTION_ID)));
            }
            s += "}";
            first = false;
            node = *reinterpret_cast<uint8_t**>(node + S_NEXT);
            ++count;
        }
        s += "]";
        s += ",\"unlock_bitmap\":" + std::to_string(*reinterpret_cast<uint16_t*>(base_ch + C_SKILL_BMP));
        uint8_t* active = *reinterpret_cast<uint8_t**>(base_ch + C_ACTIVE_SKILL);
        s += ",\"active_skill_id\":" + std::to_string(active != nullptr ? *reinterpret_cast<uint16_t*>(active + S_ACTION_ID) : -1);
        s += ",\"skill_points\":" + std::to_string(static_cast<int>(reinterpret_cast<int8_t*>(ch)[C_SKILL_POINTS]));
        s += "}";
    }
    s += "]";
    return s;
}
