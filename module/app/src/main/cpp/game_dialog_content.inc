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
        // 仅暴露「游戏结束」（game_over）一项：revive/special_revive 走网络链
        // （CS_netGetActiveNetwork），离线/无网络时无作用，故不再作为可选项提供。
        std::string out = "{\"type\":\"wipeout\",\"options\":["
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

