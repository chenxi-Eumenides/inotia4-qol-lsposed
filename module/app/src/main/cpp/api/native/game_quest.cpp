// game_quest.cpp —— 任务域：当前/列表/已完成/已接任务（parse 域）
// 由 game_misc.cpp 拆分生成（纯搬代码，零逻辑变更）。

#include "game_quest.h"

#include "game_access.h"
#include "game_state.h"
#include "game_ops_common.h"

int data_active_quest() {
    if (g_active_quest == nullptr) return -1;
    return *reinterpret_cast<uint16_t*>(g_active_quest);
}

std::string data_quest_list_json() {
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
std::string data_quest_completed_json() {
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
std::string data_quest_active_json() {
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

std::string data_op_quest_quit(int32_t quest_id) {
    if (!game_in_world()) return op_err("not in game");
    if (fn_questsystem_find == nullptr || fn_questsystem_remove_slot == nullptr)
        return op_err("symbol not resolved");
    int slot = fn_questsystem_find(quest_id);
    if (slot < 0) return op_err("quest not found");
    int r = fn_questsystem_remove_slot(slot);
    return r ? op_ok() : op_err("quest quit failed");
}
