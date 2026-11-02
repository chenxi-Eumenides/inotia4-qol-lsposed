#pragma once

#include <cstdint>
#include <string>

#include "game/access/game_access.h"
#include "game/access/ui_state.h"
#include "game/json/game_json.h"
#include "game/nav/game_nav.h"
#include "game/read/item_class.h"
#include "game/state/game_state.h"
#include "game/access/inventory_legacy_read.h"
#include "game/access/world.h"
#include "game/access/world.h"

// K2（T2i-1）：legacy 读面（原 api/native 角色/队伍 TU，T2i-2 已删除）的 host 内联副本。
// 两 TU 退役后 cache 槽的 legacy builder（插件停用 fallback）不能引用已删除符号，故把函数体
// 从 api/native 逐字搬到此处（fallback 在 B1/B2 架构下不可避免）。函数体与源逐字一致，
// 字节/线程语义零变化；`append_item_attrs` 调 game/access 现版、`append_position` 为本地内联副本（T2l-w-read2）
// （对应 TU 归整目录批），本文件不复制其主体。
namespace qol::host {

// T2l-w-read2：append_position host 内联（逐字迁自 game_world_readers.inc，避免依赖已删 TU）。
inline void append_position(std::string& s, void* member) {
    if (member != nullptr) {
        s += ",\"x\":" + std::to_string(
            *reinterpret_cast<int16_t*>(reinterpret_cast<uint8_t*>(member) + C_POS_X));
        s += ",\"y\":" + std::to_string(
            *reinterpret_cast<int16_t*>(reinterpret_cast<uint8_t*>(member) + C_POS_Y));
    } else {
        s += ",\"x\":-1,\"y\":-1";
    }
}

inline std::string legacy_member_json(void* ch) {
    std::string s = "{";
    s += "\"type\":" + std::to_string(static_cast<int>(reinterpret_cast<int8_t*>(ch)[C_TYPE]));
    s += ",\"class_idx\":" + std::to_string(static_cast<int>(reinterpret_cast<int8_t*>(ch)[C_CLASS]));
    uint16_t name_id = *reinterpret_cast<uint16_t*>(reinterpret_cast<uint8_t*>(ch) + C_NAME_ID);
    s += ",\"name_id\":" + std::to_string(name_id);
    s += ",\"level\":" + std::to_string(static_cast<int>(reinterpret_cast<int8_t*>(ch)[C_LEVEL]));
    s += ",\"hp\":" + std::to_string(*reinterpret_cast<int32_t*>(reinterpret_cast<uint8_t*>(ch) + C_HP));
    s += ",\"mp\":" + std::to_string(*reinterpret_cast<int32_t*>(reinterpret_cast<uint8_t*>(ch) + C_MP));
    // 直接读属性缓存字段（attr 0x1e/0x1f），不调用 CHAR_GetAttr（attr=0x1e 有 HP 写回副作用）
    s += ",\"max_hp\":" + std::to_string(char_max_hp(ch));
    s += ",\"max_mp\":" + std::to_string(char_max_mp(ch));
    // exp 直读 [ch+C_EXP]：CHAR_GetExperience(0xd9b54) 即 `ldr w0,[x0,#0x318]`，纯读且与函数等价。
    // exp_next 只读游戏线程帧缓存：CHAR_GetNextExperience(0xd9b68) 在 [ch+0x320]==0 时用 CAL_Calculate
    // 现算并写回（非纯读），不能在本构造线程（预取/HTTP 线程）调用；见 char_next_exp_cached。
    s += ",\"exp\":" + std::to_string(*reinterpret_cast<const int32_t*>(reinterpret_cast<const uint8_t*>(ch) + C_EXP));
    s += ",\"exp_next\":" + std::to_string(char_next_exp_cached(ch));
    s += ",\"stats\":{";
    bool first = true;
    for (int a = 0; a < 32; ++a) {
        int32_t v = *reinterpret_cast<int32_t*>(reinterpret_cast<uint8_t*>(ch) + C_ATTR + a * 4);
        if (!first) s += ",";
        s += "\"" + std::to_string(a) + "\":" + std::to_string(v);
        first = false;
    }
    s += "}";
    // main_stats 直读 Base+Main+Bonus+Sub（见 char_stat_total），避免 CHAR_GetStat 经
    // CHAR_GetStatSub 触发动态派生重算写回角色对象。
    s += ",\"main_stats\":[";
    for (int a = 0; a < 5; ++a) {
        if (a > 0) s += ",";
        s += std::to_string(char_stat_total(ch, a));
    }
    s += "]";
    // base_stats：CHAR_GetStatBase(0xdb9e4) 为纯读（add + ldrsb），保留函数调用。
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

inline std::string legacy_build_player_json() {
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

inline std::string legacy_build_skills_json() {
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

inline std::string legacy_build_party_json() {
    if (!game_in_world()) return "{\"error\":\"not in game\"}";
    std::string s = "[";
    for (int i = 0; i < 3; ++i) {
        void* ch = (fn_get_member != nullptr) ? fn_get_member(i) : nullptr;
        if (i > 0) s += ",";
        if (ch == nullptr) {
            s += "null";
        } else {
            s += legacy_member_json(ch);
        }
    }
    s += "]";
    return s;
}

inline std::string legacy_build_mercenaries_json() {
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

inline std::string legacy_build_map_json() { return qol::game::build_map_json(); }

// units/enemies/interactives 共用构建器（v0.5.35 过滤下沉 native）：
// mode 0=全量（units，含 char_loc）1=敌人（enemies，type==1）2=可交互（interactives，type==2 且 interactable==true）

inline std::string legacy_build_units_json() { return qol::game::build_units_json(); }

inline std::string legacy_build_enemies_json() { return qol::game::build_enemies_json(); }

inline std::string legacy_build_interactives_json() { return qol::game::build_interactives_json(); }

inline std::string legacy_build_drops_json() { return qol::game::build_drops_json(); }

inline std::string legacy_data_path_json(int tx, int ty) { return qol::game::data_path_json(tx, ty); }

inline std::string legacy_data_distance_json(int32_t tx, int32_t ty) { return qol::game::data_distance_json(tx, ty); }

inline std::string legacy_data_debug_path_json(int32_t tx, int32_t ty) { return qol::game::data_debug_path_json(tx, ty); }

inline std::string legacy_data_quest_list_json() {
    if (!game_in_world()) return "{\"error\":\"not in game\"}";
    std::string s = "{\"quests\":[";
    if (g_base != 0) {
        uint8_t* cnt_ptr = *reinterpret_cast<uint8_t**>(g_base + G_QUEST_SLOT_COUNT_VMA);
        uint8_t* slots_ptr = *reinterpret_cast<uint8_t**>(g_base + G_QUEST_SLOTS_GOT_VMA);
        uint8_t*** st_got = reinterpret_cast<uint8_t***>(g_base + G_NPC_QUEST_STATE_GOT_VMA);
        uint8_t* states = (st_got != nullptr && *st_got != nullptr) ? **st_got : nullptr;
        if (cnt_ptr != nullptr && slots_ptr != nullptr) {
            uint8_t cnt = *cnt_ptr;
            uint8_t* slots = *reinterpret_cast<uint8_t**>(slots_ptr);
            if (slots != nullptr && cnt > 0 && cnt <= 20) {
                bool first = true;
                for (int i = 0; i < cnt; ++i) {
                    uint16_t qid = *reinterpret_cast<uint16_t*>(slots + i * 0xC);
                    if (!first) s += ",";
                    first = false;
                    int state = (states != nullptr) ? static_cast<int>(states[qid]) : -1;
                    bool deliv = (state == 2);
                    s += "{\"slot\":" + std::to_string(i) + ",\"quest_id\":" + std::to_string(qid) +
                         ",\"deliverable\":" + (deliv ? "true" : "false") + "}";
                }
            }
        }
    }
    s += "]}";
    return s;
}

// v0.5.4：已完成任务列表（Q3）——遍历 G_NPC_QUEST_STATE 状态表过滤 state==3
inline std::string legacy_data_quest_completed_json() {
    if (!game_in_world()) return "{\"error\":\"not in game\"}";
    std::string s = "{\"quests\":[";
    if (g_base != 0) {
        uint16_t* qcnt_var = *reinterpret_cast<uint16_t**>(g_base + G_QUEST_COUNT_GOT_VMA);
        uint8_t*** st_got = reinterpret_cast<uint8_t***>(g_base + G_NPC_QUEST_STATE_GOT_VMA);
        if (qcnt_var != nullptr && st_got != nullptr && *st_got != nullptr && **st_got != nullptr) {
            uint16_t qcnt = *qcnt_var;
            uint8_t* states = **st_got;
            bool first = true;
            if (qcnt > 0 && qcnt <= 512) {
                for (uint16_t qid = 0; qid < qcnt; ++qid) {
                    if (states[qid] != 3) continue;
                    if (!first) s += ",";
                    first = false;
                    s += "{\"quest_id\":" + std::to_string(qid) + "}";
                }
            }
        }
    }
    s += "]}";
    return s;
}

    // v0.5.5：已接任务列表（Q2）——槽数组（QUESTSYSTEM_Find 0x12292c 遍历，12B/槽 +0 questId，G_QUEST_SLOTS_GOT_VMA）
// + G_NPC_QUEST_STATE 状态表（0 未接/1 进行/2 可完成/3 已完成）
inline std::string legacy_data_quest_active_json() {
    if (!game_in_world()) return "{\"error\":\"not in game\"}";
    std::string s = "{\"quests\":[";
    if (g_base != 0) {
        uint8_t* cnt_ptr = *reinterpret_cast<uint8_t**>(g_base + G_QUEST_SLOT_COUNT_VMA);
        uint8_t* slots_ptr = *reinterpret_cast<uint8_t**>(g_base + G_QUEST_SLOTS_GOT_VMA);
        uint8_t*** st_got = reinterpret_cast<uint8_t***>(g_base + G_NPC_QUEST_STATE_GOT_VMA);
        uint8_t* states = (st_got != nullptr && *st_got != nullptr) ? **st_got : nullptr;
        if (cnt_ptr != nullptr && slots_ptr != nullptr && cnt_ptr != slots_ptr) {
            uint8_t cnt = *cnt_ptr;
            uint8_t* slots = *reinterpret_cast<uint8_t**>(slots_ptr);
            if (slots != nullptr && cnt > 0 && cnt <= 20) {
                bool first = true;
                for (int i = 0; i < cnt; ++i) {
                    uint16_t qid = *reinterpret_cast<uint16_t*>(slots + i * 0xC);
                    if (!first) s += ",";
                    first = false;
                    int state = (states != nullptr) ? static_cast<int>(states[qid]) : -1;
                    bool deliv = (state == 2);
                    s += "{\"slot\":" + std::to_string(i) + ",\"quest_id\":" + std::to_string(qid) +
                         ",\"deliverable\":" + (deliv ? "true" : "false") + "}";
                }
            }
        }
    }
    s += "]}";
    return s;
}

inline std::string legacy_data_debug_ui_json() {
    char buf[4096];
    uint16_t state = g_state ? *reinterpret_cast<uint16_t*>(g_state) : 0xFFFF;
    uint16_t prev = g_prev_state ? *reinterpret_cast<uint16_t*>(g_prev_state) : 0xFFFF;
    uint32_t gs = g_gamestate ? *reinterpret_cast<uint32_t*>(g_gamestate) : 0xFFFFFFFF;
    uint8_t init = g_initstate ? *reinterpret_cast<uint8_t*>(g_initstate) : 0xFF;
    uint8_t popup_on = g_popup_on ? *reinterpret_cast<uint8_t*>(g_popup_on) : 0xFF;
    uint8_t menu_draw = g_mainmenu_draw ? *reinterpret_cast<uint8_t*>(g_mainmenu_draw) : 0xFF;

    snprintf(buf, sizeof(buf),
        "{"
        "\"state\":%u,\"prev_state\":%u,\"gamestate\":%u,\"initstate\":%u,"
        "\"popup_on\":%u,\"menu_draw\":%u,"
        "\"popup_stack\":[",
        state, prev, gs, init, popup_on, menu_draw);

    std::string s = buf;
    if (g_popup_stack) {
        uint8_t* stk = reinterpret_cast<uint8_t*>(g_popup_stack);
        for (int i = 0; i < 32; i += 4) {
            if (i > 0) s += ",";
            s += std::to_string(*reinterpret_cast<uint32_t*>(stk + i));
        }
    } else {
        s += "null";
    }
    s += "],\"popup_stack_hex\":\"";
    if (g_popup_stack) {
        uint8_t* stk = reinterpret_cast<uint8_t*>(g_popup_stack);
        for (int i = 0; i < 32; ++i) {
            snprintf(buf, sizeof(buf), "%02x", stk[i]);
            s += buf;
        }
    }
    s += "\"";

    if (g_base != 0) {
        int32_t i32type = *reinterpret_cast<int32_t*>(g_base + G_POPUP_TYPE_VMA);
        int32_t i32disp  = *reinterpret_cast<int32_t*>(g_base + G_POPUP_DISPTYPE_VMA);
        s += ",\"popup_type\":" + std::to_string(i32type);
        s += ",\"popup_disp_type\":" + std::to_string(i32disp);

        auto r8 = [&](uintptr_t vma, const char* name) {
            s += ",\"" + std::string(name) + "\":" +
                 std::to_string(static_cast<int>(*reinterpret_cast<int8_t*>(g_base + vma)));
        };
        auto ru8 = [&](uintptr_t vma, const char* name) {
            s += ",\"" + std::string(name) + "\":" +
                 std::to_string(static_cast<int>(*reinterpret_cast<uint8_t*>(g_base + vma)));
        };
        auto ru16 = [&](uintptr_t vma, const char* name) {
            s += ",\"" + std::string(name) + "\":" +
                 std::to_string(*reinterpret_cast<uint16_t*>(g_base + vma));
        };
        auto ru32 = [&](uintptr_t vma, const char* name) {
            s += ",\"" + std::string(name) + "\":" +
                 std::to_string(*reinterpret_cast<uint32_t*>(g_base + vma));
        };

        r8(G_UI_PARTY_MENU_INDEX_VMA, "partyMenuIndex");
        ru8(G_UI_QUEST_MENU_STATE_VMA, "questMenuState");
        ru8(G_UI_STORE_BUY_TYPE_VMA, "storeBuyType");
        ru8(G_UI_STORE_SEL_CLASS_VMA, "storeSelectedClass");
        ru8(G_UI_HELP_STATE_VMA, "helpState");
        ru8(G_UI_MMENU_SEL_CLASS_VMA, "mainmenuSelectedClass");
        ru8(G_UI_MMENU_SAVE_SLOT_VMA, "mainmenuSaveSlotType");
        ru8(G_UICHOICE_FOCUS_VMA, "choiceFocusIndex");
        ru8(G_UI_SHORTCUT_PAGE_VMA, "shortcutPage");
        ru16(G_UI_QUEST_MENU_MAIN_SIZE_VMA, "questMenuMainListSize");
        ru16(G_UI_QUEST_MENU_SUB_SIZE_VMA, "questMenuSubListSize");
        ru32(G_POPUP_FPCANCEL_VMA, "popupFpCancelLo");
    }
    // popup 栈顶原始字段（诊断用）：popup_top_vma 只返回「减去 g_base 后的 VMA」，
    // 当 enter 字段本身不是绝对地址时该换算会得到 0，无法区分「栈空」与「enter 非绝对地址」。
    if (g_popup_stack != nullptr) {
        uint8_t* stk = reinterpret_cast<uint8_t*>(g_popup_stack);
        uint32_t count = *reinterpret_cast<uint32_t*>(stk + POPUP_STACK_COUNT);
        uint64_t data = *reinterpret_cast<uint64_t*>(stk + POPUP_STACK_DATA);
        s += ",\"popup_count\":" + std::to_string(count);
        s += ",\"popup_data\":" + std::to_string(data);
        s += ",\"g_base\":" + std::to_string(g_base);
        if (data != 0 && count > 0 && count <= 27) {
            uint8_t* top = reinterpret_cast<uint8_t*>(static_cast<uintptr_t>(data)) + (count - 1) * POPUP_ENTRY_SIZE;
            s += ",\"popup_top_enter\":" +
                 std::to_string(*reinterpret_cast<uint64_t*>(top + POPUP_ENTRY_ENTER));
            s += ",\"popup_top_enter_vma\":" + std::to_string(qol::game::popup_top_vma());
        }
    }

    s += "}";
    return s;
}

}  // namespace qol::host
