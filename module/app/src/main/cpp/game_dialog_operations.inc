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
