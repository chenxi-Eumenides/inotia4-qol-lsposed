// game_dialog.cpp —— 对话域：NPC 对话框选项 + 对话内容（parse 域）
// 由 game_misc.cpp 拆分生成（纯搬代码，零逻辑变更）。

#include "game_dialog.h"

#include "game_access.h"
#include "game_json.h"
#include "game_state.h"
#include "game_ui.h"
#include "game_world.h"
#include "game_save.h"
#include "game_ops_common.h"
#include "game_cache.h"

#include <cstring>

std::string data_npc_dialog_options_json() {
    if (!game_in_world()) return "{\"error\":\"not in game\"}";
    std::string out = "{\"count\":" +
        std::to_string(static_cast<int>(*reinterpret_cast<uint8_t*>(g_base + G_UICHOICE_COUNT_VMA)));
    out += ",\"focus\":" +
        std::to_string(static_cast<int>(*reinterpret_cast<uint8_t*>(g_base + G_UICHOICE_FOCUS_VMA)));
    out += ",\"options\":[";
    void** texts = reinterpret_cast<void**>(g_base + G_UICHOICE_ITEMTEXT_VMA);
    for (int i = 0; i < 6; ++i) {
        if (i > 0) out += ",";
        char* t = reinterpret_cast<char*>(texts[i]);
        if (t != nullptr) {
            out += "\"" + json_escape(t) + "\"";
        } else {
            out += "null";
        }
    }
    out += "]}";
    return out;
}

std::string data_dialog_content_json() {
    // 弹窗最优先（v0.4.39 修复）：剧情段结束弹任务简报时 gs=1 残留但 UIPopupMsg 激活，
    // 若 story 优先会遮蔽弹窗 → select ok 无法确认任务简报。弹窗会阻塞一切下层交互。
    if (g_base != 0 && g_popup_on != nullptr && *reinterpret_cast<uint8_t*>(g_popup_on)) {
        std::string out = "{\"type\":\"popup\"";
        uint8_t* pt = *reinterpret_cast<uint8_t**>(g_base + G_POPUP_TEXT_VMA);
        if (pt != nullptr) {
            std::string dtext;
            for (int i = 0; i < 256 && pt[i] != 0; ++i) dtext += static_cast<char>(pt[i]);
            out += ",\"text\":\"" + json_escape(dtext.c_str()) + "\"";
        }
        bool has_ok = *reinterpret_cast<uint64_t*>(g_base + G_POPUP_FPOK_VMA) != 0;
        bool has_cancel = *reinterpret_cast<uint64_t*>(g_base + G_POPUP_FPCANCEL_VMA) != 0;
        out += ",\"options\":[";
        bool first = true;
        if (has_ok) { out += "{\"id\":\"ok\",\"label\":\"确认\"}"; first = false; }
        if (has_cancel) { if (!first) out += ","; out += "{\"id\":\"cancel\",\"label\":\"取消\"}"; }
        out += "]}";
        return out;
    }
    if (!game_in_world()) return "{\"error\":\"not in game\"}";
    if (data_story_active()) {
        // story 态：统一 type 字段 + 剧情推进/跳过作为选项暴露（v0.4.31 修复）
        std::string sj = data_story_json();
        if (sj.size() > 1 && sj[0] == '{') {
            sj.insert(1, "\"type\":\"story\",\"options\":[{\"id\":\"next\",\"label\":\"下一句\"},{\"id\":\"skip\",\"label\":\"跳过\"}],");
        }
        return sj;
    }
    if (g_base == 0) return "{\"type\":\"none\",\"options\":[]}";
    // NPC 对话（UICHOICE 选项优先）
    uint8_t choice_count = *reinterpret_cast<uint8_t*>(g_base + G_UICHOICE_COUNT_VMA);
    uint8_t task_count = *reinterpret_cast<uint8_t*>(g_base + G_NPCTASKLIST_COUNT_VMA);
    // wipeout 死亡面板（v0.4.35）：栈顶 enter == F_PANEL_WIPEOUT_ENTER 时优先于 NPC 对话报告
    uintptr_t top_vma = data_popup_top_vma();
    if (top_vma == F_PANEL_WIPEOUT_ENTER) {
        std::string out = "{\"type\":\"wipeout\",\"options\":["
                          "{\"id\":\"revive\",\"label\":\"复活\"},"
                          "{\"id\":\"special_revive\",\"label\":\"特殊复活\"},"
                          "{\"id\":\"game_over\",\"label\":\"游戏结束\"}]}";
        return out;
    }
    // NPC 任务完成面板（v0.4.55）：栈顶 enter == F_PANEL_NPC_QUEST_ENTER（npc_quest）时报告任务完成态，
    // 选项 complete=完成任务（UINpcQuest_ButtonOKExe 官方链）close=关闭面板（panel/close）。
    if (top_vma == F_PANEL_NPC_QUEST_ENTER) {
        std::string out = "{\"type\":\"npc_quest\"";
        int quest_id = -1;
        if (g_base != 0) {
            uint8_t** idx_ptr = reinterpret_cast<uint8_t**>(g_base + G_NPC_QUEST_IDX_GOT_VMA);
            if (idx_ptr != nullptr && *idx_ptr != nullptr)
                quest_id = *reinterpret_cast<int16_t*>(*idx_ptr);
        }
        out += ",\"quest_id\":" + std::to_string(quest_id);
        uint8_t state = 0xFF;
        if (g_base != 0 && quest_id >= 0) {
            uint8_t*** st_got = reinterpret_cast<uint8_t***>(g_base + G_NPC_QUEST_STATE_GOT_VMA);
            if (st_got != nullptr && *st_got != nullptr)
                state = (**st_got)[quest_id];
        }
        out += ",\"state\":" + std::to_string(static_cast<int>(state));
        out += ",\"options\":[{\"id\":\"complete\",\"label\":\"完成任务\"},"
               "{\"id\":\"close\",\"label\":\"关闭\"}]}";
        return out;
    }
    if (choice_count > 0 || task_count > 0) {
        std::string out = "{\"type\":\"npc\"";
        // displayed：区分「UI 对话框已显示」（popup 栈顶 npc 面板）vs「数据已建立但未渲染」
        // （如 start_interact 前的数据态、路过不可交互装饰物残留的 NEAR_NPC 数据）。
        out += ",\"displayed\":" +
               std::string(data_popup_top_vma() == F_PANEL_NPC_ENTER ? "true" : "false");
        void* near_npc = *reinterpret_cast<void**>(g_base + G_PLAYER_NEAR_NPC_VMA);
        if (near_npc != nullptr && fn_get_name != nullptr) {
            char* nm = fn_get_name(near_npc);
            if (nm != nullptr) out += ",\"speaker\":\"" + json_escape(nm) + "\"";
        }
        char* desc = *reinterpret_cast<char**>(g_base + G_NPCTASKLIST_DESCTEXT_VMA);
        if (desc != nullptr && desc[0] != 0) {
            out += ",\"text\":\"" + json_escape(desc) + "\"";
        }
        out += ",\"options\":[";
        if (choice_count > 0) {
            void** texts = reinterpret_cast<void**>(g_base + G_UICHOICE_ITEMTEXT_VMA);
            for (int i = 0; i < choice_count && i < 6; ++i) {
                if (i > 0) out += ",";
                char* t = reinterpret_cast<char*>(texts[i]);
                out += "{\"id\":\"" + std::to_string(i) + "\",\"label\":" +
                       (t != nullptr ? "\"" + json_escape(t) + "\"" : "\"\"") + "}";
            }
        } else if (task_count > 0) {
            // NPC 选项列表（NPCTASKLIST 槽数组：32×16B，+0 type/+2 id/+8 文本指针）。
            // 选择框型 NPC（如商人）选项在此，非线性 next（v0.6.6 修复：原误判为 next）。
            void* slots = *reinterpret_cast<void**>(g_base + G_NPCTASKLIST_PDATA_VMA);
            bool first = true;
            for (int i = 0; i < task_count && i < 32; ++i) {
                char* label = slots != nullptr
                    ? *reinterpret_cast<char**>(reinterpret_cast<uint8_t*>(slots) + i * 16 + 8)
                    : nullptr;
                if (!first) out += ",";
                out += "{\"id\":\"" + std::to_string(i) + "\",\"label\":" +
                       (label != nullptr ? "\"" + json_escape(label) + "\"" : "\"\"") + "}";
                first = false;
            }
            if (!first) out += ",";
            out += "{\"id\":\"close\",\"label\":\"关闭\"}";
        }
        out += "]}";
        return out;
    }
    // 面板态（v0.5.6 U1）：无弹窗/剧情/死亡/NPC 对话时，若栈顶是面板则报面板类型 + 动作。
    // save_slot 额外暴露 save（存档落盘，data_op_save 官方链）；其余面板仅 close（panel/close 官方流程3）。
    const char* pname = data_top_panel_name();
    if (pname != nullptr) {
        std::string out = "{\"type\":\"" + std::string(pname) + "\",\"options\":[";
        if (strcmp(pname, "save_slot") == 0)
            out += "{\"id\":\"save\",\"label\":\"存档\"},";
        out += "{\"id\":\"close\",\"label\":\"关闭\"}]}";
        return out;
    }
    return "{\"type\":\"none\",\"options\":[]}";
}

std::string data_op_npc_interact() {
    if (!game_in_world()) return op_err("not in game");
    if (const char* ui = ui_blocked()) {
        std::string err = "ui occupied: ";
        err += ui;
        return op_err(err.c_str());
    }
    if (fn_player_check_near_npc == nullptr || fn_uinpc_init == nullptr ||
        fn_check_function_display == nullptr)
        return op_err("symbol not resolved");
    fn_player_check_near_npc();
    void* near_npc = *reinterpret_cast<void**>(g_base + G_PLAYER_NEAR_NPC_VMA);
    // v0.4.52：NearNPC 空时模拟触摸交互链——扫描角色池找玩家面前/附近的 type==2 可交互物
    // （路障/宝箱）。官方触摸链不经过 NearNPC 24px 检测，直接检测点击处对象；玩家距路障 32px
    // 超阈值但触摸可交互（b54 真机：手动点击成功建立路障对话）。这里放宽到 3 格（48px）且朝向匹配。
    if (near_npc == nullptr && g_base != 0) {
        void* hero = lead_member();
        if (hero != nullptr) {
            int hx = *reinterpret_cast<int16_t*>(reinterpret_cast<uint8_t*>(hero) + C_POS_X);
            int hy = *reinterpret_cast<int16_t*>(reinterpret_cast<uint8_t*>(hero) + C_POS_Y);
            int hdir = *reinterpret_cast<uint8_t*>(reinterpret_cast<uint8_t*>(hero) + 0x6);
            uint8_t* pool = *reinterpret_cast<uint8_t**>(g_base + G_CHAR_POOL_VMA);
            if (pool != nullptr) {
                int best_dist = 60, best_slot = -1;
                for (int i = 0; i < C_CHARSYSTEM_POOL_SLOTS; ++i) {
                    uint8_t* obj = pool + i * C_OBJ_SIZE;
                    int16_t x = *reinterpret_cast<int16_t*>(obj + C_POS_X);
                    int16_t y = *reinterpret_cast<int16_t*>(obj + C_POS_Y);
                    int type = static_cast<int>(reinterpret_cast<int8_t*>(obj)[C_TYPE]);
                    if (type != 2) continue;
                    if (x <= 0 || x >= 1500 || y <= 0 || y >= 1500) continue;
                    int dx = x - hx, dy = y - hy;
                    int dist = (dx < 0 ? -dx : dx) + (dy < 0 ? -dy : dy);
                    if (dist > 60) continue;
                    // 朝向匹配：dir 0=下 1=左 2=上 3=右（dx/dy 符号对应）
                    bool facing = (hdir == 3 && dx > 0) || (hdir == 1 && dx < 0) ||
                                  (hdir == 0 && dy > 0) || (hdir == 2 && dy < 0);
                    if (!facing) continue;
                    if (dist < best_dist) { best_dist = dist; best_slot = i; }
                }
                if (best_slot >= 0) {
                    near_npc = pool + best_slot * C_OBJ_SIZE;
                    *reinterpret_cast<void**>(g_base + G_PLAYER_NEAR_NPC_VMA) = near_npc;
                }
            }
        }
        if (near_npc == nullptr) {
            if (fn_evtsystem_do_check_all_event == nullptr) return op_err("no npc nearby");
            fn_evtsystem_do_check_all_event(2);
            return op_ok();
        }
    }
    // 官方交互链（GAMESTATE_PressKeyPlay 交互分支）：读 npc+0xa u16 funcDisplay，
    // NPCSYSTEM_CheckFunctionDisplay 返回 0=弹 UI / 1=直接执行任务 / 2=不可交互。
    uint16_t func_display = *reinterpret_cast<uint16_t*>(reinterpret_cast<uint8_t*>(near_npc) + 0xa);
    int fd = fn_check_function_display(static_cast<int32_t>(func_display));
    if (fd > 1) return op_err("not interactable");
    uint8_t r = fn_uinpc_init();
    if (!r) return op_err("interact failed");
    frame_cache_force_refresh();
    if (fd == 0) {
        int state_id = -1;
        uint8_t* list = *reinterpret_cast<uint8_t**>(g_base + G_POPUP_STATE_LIST_GOT_VMA);
        if (list != nullptr) {
            for (int i = 0; i < 27; ++i) {
                uintptr_t enter = *reinterpret_cast<uintptr_t*>(list + i * 0x40 + 0x10);
                if (enter == g_base + F_PANEL_NPC_ENTER) { state_id = i; break; }
            }
        }
        if (state_id < 0) return op_err("npc panel state not found");
        if (fn_ui_set_popup_process_info == nullptr) return op_err("symbol not resolved");
        fn_ui_set_popup_process_info(1, state_id);
        return "{\"ok\":true,\"result\":\"dialog_shown\"}";
    }
    if (fn_uinpc_exe_current_task == nullptr) return op_err("symbol not resolved");
    fn_uinpc_exe_current_task();
    return "{\"ok\":true,\"result\":\"task_executed\"}";
}
std::string data_op_npc_dialog_select(int index) {
    if (!game_in_world()) return op_err("not in game");
    if (fn_uinpc_exe_current_task == nullptr) return op_err("symbol not resolved");
    uint8_t count = *reinterpret_cast<uint8_t*>(g_base + G_NPCTASKLIST_COUNT_VMA);
    if (index < 0 || index >= count) return op_err("bad index");
    *reinterpret_cast<uint8_t*>(g_base + G_NPCTASKLIST_INDEX_VMA) = static_cast<uint8_t>(index);
    fn_uinpc_exe_current_task();
    return op_ok();
}
std::string data_op_dialog_select(const std::string& action, int index) {
    if (!game_in_world()) return op_err("not in game");
    // 先按当前态确定合法 action 集合（与 data_dialog_content_json 判定一致：popup 最优先）
    if (g_popup_on != nullptr && *reinterpret_cast<uint8_t*>(g_popup_on)) {
        if (action != "ok" && action != "cancel") return op_err("no such option in popup");
    } else if (data_story_active()) {
        if (action != "next" && action != "skip") return op_err("no such option in story");
    } else if (data_popup_top_vma() == F_PANEL_WIPEOUT_ENTER) {
        if (action != "revive" && action != "special_revive" && action != "game_over")
            return op_err("no such option in wipeout");
    } else if (data_popup_top_vma() == F_PANEL_NPC_QUEST_ENTER) {
        // npc_quest 面板：仅接受 complete（完成任务）/ close（关闭面板）
        if (action != "complete" && action != "close") return op_err("no such option in npc_quest");
    } else if (g_base != 0 &&
               (*reinterpret_cast<uint8_t*>(g_base + G_UICHOICE_COUNT_VMA) > 0 ||
                *reinterpret_cast<uint8_t*>(g_base + G_NPCTASKLIST_COUNT_VMA) > 0)) {
        if (action != "next" && action != "close" && index < 0) return op_err("no such option in npc");
    } else if (data_top_panel_name() != nullptr) {
        // 面板态（v0.5.6 U1）：save_slot 接受 save/close，其余面板仅 close（panel/close 官方流程3）
        if (action != "close" && !(action == "save" && strcmp(data_top_panel_name(), "save_slot") == 0)) {
            std::string err = "no such option in ";
            err += data_top_panel_name();
            return op_err(err.c_str());
        }
    } else {
        return op_err("no dialog");
    }
    if (action == "next") {
        if (data_story_active()) return story_next();
        // 非 story 的 next（NPC/任务列表对话）：用与 get-content 同源的安全文本读取
        // （G_NPCTASKLIST_DESCTEXT_VMA 指向当前对话文本），避免 fn_npctasklist_make_dlg
        // 对非 NPC 对象（如路障交互）返回悬垂指针导致 json_escape 越界崩溃（v0.4.48 修复）
        char* desc = nullptr;
        if (g_base != 0) desc = *reinterpret_cast<char**>(g_base + G_NPCTASKLIST_DESCTEXT_VMA);
        if (desc == nullptr || desc[0] == 0) return op_err("no dialog");
        return "{\"ok\":true,\"text\":\"" + json_escape(desc) + "\"}";
    }
    if (action == "skip") {
        if (!data_story_active()) return op_err("no story");
        return story_skip();
    }
    if (action == "ok") return data_op_dialog_ok();
    if (action == "cancel") return data_op_dialog_cancel();
    // npc_quest 面板动作（v0.4.55）：complete=完成任务（UINpcQuest_ButtonOKExe 官方链：
    // questIdx→stateTbl 判定→完成态走 UI_SetPopupProcessInfo(3,0)+ChangeQuestState+DoCheckAllEvent），
    // close=复用面板关闭（panel/close 官方流程3）。
    if (data_popup_top_vma() == F_PANEL_NPC_QUEST_ENTER) {
        if (action == "complete") {
            if (fn_uinpc_quest_button_ok_exe == nullptr) return op_err("symbol not resolved");
            fn_uinpc_quest_button_ok_exe();
            return op_ok();
        }
        if (action == "close") return data_op_panel_close();
    }
    // wipeout 死亡面板动作（v0.4.35）：栈顶是 wipeout 面板时接受 revive/special_revive/game_over
    if (action == "revive" || action == "special_revive" || action == "game_over") {
        if (data_popup_top_vma() != F_PANEL_WIPEOUT_ENTER) return op_err("not in wipeout");
        if (action == "revive") {
            if (fn_wipeout_button_revive == nullptr) return op_err("symbol not resolved");
            fn_wipeout_button_revive();
        } else if (action == "special_revive") {
            if (fn_wipeout_button_special_revive == nullptr) return op_err("symbol not resolved");
            fn_wipeout_button_special_revive();
        } else {
            if (fn_wipeout_button_gameover == nullptr) return op_err("symbol not resolved");
            fn_wipeout_button_gameover();
        }
        return op_ok();
    }
    if (action == "close") return data_op_panel_close();
    if (index >= 0) return data_op_npc_dialog_select(index);
    // 面板态动作（v0.5.6 U1）：save=存档落盘、close=关闭面板（panel/close 官方流程3）
    if (data_top_panel_name() != nullptr) {
        if (action == "save") return data_op_save();
        if (action == "close") return data_op_panel_close();
    }
    return op_err("bad action");
}
std::string data_op_dialog_ok() {
    if (g_base == 0) return op_err("libgame not loaded");
    if (*reinterpret_cast<uint8_t*>(g_base + G_POPUP_ON_VMA) == 0) return op_err("no dialog");
    reinterpret_cast<void (*)()>(g_base + F_BUTTON_OK_EXE_VMA)();
    return op_ok();
}
std::string data_op_dialog_cancel() {
    if (g_base == 0) return op_err("libgame not loaded");
    if (*reinterpret_cast<uint8_t*>(g_base + G_POPUP_ON_VMA) == 0) return op_err("no dialog");
    reinterpret_cast<void (*)()>(g_base + F_BUTTON_CANCEL_EXE_VMA)();
    return op_ok();
}
std::string story_next() {
    if (g_base == 0) return op_err("not in game");
    if (fn_event_button_ok_exe == nullptr) return op_err("symbol not resolved");
    fn_event_button_ok_exe();
    return op_ok();
}
std::string story_skip() {
    if (g_base == 0) return op_err("not in game");
    if (fn_event_button_skip_exe == nullptr) return op_err("symbol not resolved");
    fn_event_button_skip_exe();
    return op_ok();
}
