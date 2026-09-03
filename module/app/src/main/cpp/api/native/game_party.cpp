// game_party.cpp —— 队伍域：队伍编成操作（parse 域）
// 由 game_ops_action.cpp 拆分生成（纯搬代码，零逻辑变更）。

#include "game_party.h"

#include "game_access.h"
#include "game_state.h"
#include "game_ops_common.h"
#include "game_character.h"
#include "game_json.h"
#include "game_world.h"

#include <cstdint>
#include <string>

std::string data_op_party_swap(int32_t a, int32_t b) {
    if (!game_in_world()) return op_err("not in game");
    if (fn_party_swap == nullptr) return op_err("symbol not resolved");
    if (a < 0 || a > 2 || b < 0 || b > 2) return op_err("bad slot");
    fn_party_swap(a, b);
    return op_ok();
}
std::string data_op_include_party(int mercenary_slot) {
    if (!game_in_world()) return op_err("not in game");
    if (fn_include_party == nullptr || fn_get_party_size == nullptr || fn_get_member == nullptr)
        return op_err("symbol not resolved");
    void* ch = find_char_by_merc_slot(mercenary_slot);
    if (ch == nullptr) return op_err("mercenary not found");
    // 前置校验（避免触发游戏弹窗）：目标已在队则不调游戏函数，其次检查满员
    for (int i = 0; i < 3; ++i) {
        if (fn_get_member(i) == ch) return op_err("already in party");
    }
    if (fn_get_party_size() >= 3) return op_err("party full");
    int r = fn_include_party(ch);
    return r ? op_ok() : op_err("party full or include failed");
}
std::string data_op_exclude_party(int mercenary_slot) {
    if (!game_in_world()) return op_err("not in game");
    if (fn_exclude_party == nullptr || fn_get_member == nullptr)
        return op_err("symbol not resolved");
    void* ch = find_char_by_merc_slot(mercenary_slot);
    if (ch == nullptr) return op_err("mercenary not found");
    // 前置校验（避免触发游戏弹窗/破坏剧情）：主控或任务特殊 NPC 不能离队
    if (fn_get_member(0) == ch) return op_err("cannot exclude leader");
    if (fn_is_special_npc != nullptr && fn_is_special_npc(ch)) return op_err("cannot exclude quest npc");
    int r = fn_exclude_party(ch);
    return r ? op_ok() : op_err("exclude failed");
}
std::string data_op_discharge(int mercenary_slot) {
    if (!game_in_world()) return op_err("not in game");
    if (fn_mercenary_release == nullptr || fn_get_member == nullptr)
        return op_err("symbol not resolved");
    void* ch = find_char_by_merc_slot(mercenary_slot);
    if (ch == nullptr) return op_err("mercenary not found");
    if (fn_get_member(0) == ch) return op_err("cannot discharge leader");
    if (fn_is_special_npc != nullptr && fn_is_special_npc(ch)) return op_err("cannot discharge quest npc");
    fn_mercenary_release(mercenary_slot);
    return op_ok();
}
std::string data_op_withdraw(int mercenary_slot, int32_t equip_slot) {
    if (!game_in_world()) return op_err("not in game");
    if (fn_unequip == nullptr) return op_err("symbol not resolved");
    void* ch = find_char_by_merc_slot(mercenary_slot);
    if (ch == nullptr) return op_err("mercenary not found");
    if (equip_slot < 0 || equip_slot >= C_EQUIP_SLOTS) return op_err("bad slot");
    int r = fn_unequip(ch, equip_slot);
    return r ? op_ok() : op_err("withdraw failed");
}
std::string data_op_switch_player(int32_t slot) {
    if (!game_in_world()) return op_err("not in game");
    if (fn_set_active_player == nullptr) return op_err("symbol not resolved");
    if (slot < 0 || slot > 2) return op_err("bad slot");
    int r = fn_set_active_player(slot);
    if (!r) return op_err("switch failed");
    // v0.5.9：PARTY_SetActivePlayer 只写 PLAYER_pActivePlayer，不同步 SAVE_nMainMercenarySlot，
    // 导致 main_mercenary_slot/leader 端点不更新（2026-08-13 实测修复）
    if (g_main_merc_slot != nullptr)
        *reinterpret_cast<uint8_t*>(g_main_merc_slot) = static_cast<uint8_t>(slot);
    return op_ok();
}

std::string build_party_json() {
    if (!game_in_world()) return "{\"error\":\"not in game\"}";
    std::string s = "[";
    for (int i = 0; i < 3; ++i) {
        void* ch = (fn_get_member != nullptr) ? fn_get_member(i) : nullptr;
        if (i > 0) s += ",";
        if (ch == nullptr) {
            s += "null";
        } else {
            s += member_json(ch);
        }
    }
    s += "]";
    return s;
}

std::string build_mercenaries_json() {
    if (!game_in_world()) return "{\"error\":\"not in game\"}";
    // 佣兵槽：*(*(G_MERC_SLOTLIST_GOT_VMA)) 槽数组（20B/槽），flags bit0=占用 bit1=在队伍。
    // 关联角色：角色池 +0x352==slot；名称 CHAR_GetName、等级 +0x0E、坐标 +0x02/+0x04。
    std::string s = "[";
    if (g_base != 0) {
        uintptr_t got = *reinterpret_cast<uintptr_t*>(g_base + G_MERC_SLOTLIST_GOT_VMA);
        uint8_t* slots = got != 0 ? *reinterpret_cast<uint8_t**>(got) : nullptr;
        uintptr_t max_got = *reinterpret_cast<uintptr_t*>(g_base + G_MERC_MAX_GOT_VMA);
        int8_t max_slots = max_got != 0 ? *reinterpret_cast<int8_t*>(max_got) : 0;
        if (slots != nullptr && max_slots > 0) {
            int emitted = 0;
            for (int i = 0; i < max_slots && i < 128; ++i) {
                uint8_t* slot = slots + i * M_SLOT_SIZE;
                uint8_t flags = slot[M_FLAGS];
                if ((flags & 0x01) == 0) continue;
                if (slot[M_TYPE] > 2) continue;
                if (emitted > 0) s += ",";
                s += "{\"slot\":" + std::to_string(i);
                s += ",\"type\":" + std::to_string(slot[M_TYPE]);
                s += ",\"flags\":" + std::to_string(flags);
                s += ",\"in_party\":" + std::string((flags & 0x02) ? "true" : "false");
                void* ch = find_char_by_merc_slot(i);
                if (ch != nullptr) {
                    if (fn_get_name != nullptr) {
                        char* nm = fn_get_name(ch);
                        s += ",\"name\":\"" + json_escape(nm) + "\"";
                    }
                    s += ",\"level\":" + std::to_string(static_cast<int>(reinterpret_cast<int8_t*>(ch)[C_LEVEL]));
                    append_position(s, ch);
                } else {
                    s += ",\"name\":null";
                }
                s += "}";
                ++emitted;
            }
        }
    }
    s += "]";
    return s;
}
