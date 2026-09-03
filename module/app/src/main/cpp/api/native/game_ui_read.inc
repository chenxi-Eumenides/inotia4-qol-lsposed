std::string data_debug_ui_json() {
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

    s += "}";
    return s;
}

uintptr_t data_popup_top_vma() {
    if (g_popup_stack == nullptr || g_base == 0) return 0;
    uint8_t* stk = reinterpret_cast<uint8_t*>(g_popup_stack);
    uint32_t count = *reinterpret_cast<uint32_t*>(stk + 8);
    if (count == 0 || count > 27) return 0;
    uint64_t data = *reinterpret_cast<uint64_t*>(stk + 0x18);
    if (data == 0) return 0;
    uint8_t* top = reinterpret_cast<uint8_t*>(static_cast<uintptr_t>(data)) + (count - 1) * 0x40;
    uintptr_t enter = *reinterpret_cast<uintptr_t*>(top + 0x10);
    if (extension_bag_is_inventory_enter(enter)) return F_PANEL_INVENTORY_ENTER;
    return enter > g_base ? enter - g_base : 0;
}

const char* data_top_panel_name() {
    uintptr_t vma = data_popup_top_vma();
    switch (vma) {
        case F_PANEL_CHARACTER_INFO_ENTER: return "character_info";
        case F_PANEL_CHOICE_ENTER: return "choice";
        case F_PANEL_INVENTORY_ENTER: return "inventory";
        case F_PANEL_INPUT_COUNT_ENTER: return "input_count";
        case F_PANEL_MERCENARY_ENTER: return "mercenary";
        case F_PANEL_CRAFT_ENTER: return "craft";
        case F_PANEL_NPC_ENTER: return "npc";
        case F_PANEL_NPC_QUEST_ENTER: return "npc_quest";
        case F_PANEL_NPC_REST_ENTER: return "npc_rest";
        case F_PANEL_NPC_REVIVE_ENTER: return "npc_revive";
        case F_PANEL_OPTIONS_ENTER: return "options";
        case F_PANEL_QUESTS_ENTER: return "quests";
        case F_PANEL_SAVE_SLOT_ENTER: return "save_slot";
        case F_PANEL_CHAR_SELECT_ENTER: return "character_select";
        case F_PANEL_SHORTCUT_ENTER: return "shortcut";
        case F_PANEL_SKILLS_ENTER: return "skills";
        case F_PANEL_SHOP_ENTER: return "shop";
        case F_PANEL_SETTINGS_ENTER: return "settings";
        case F_PANEL_WIPEOUT_ENTER: return "wipeout";
        case F_PANEL_WORLD_MAP_ENTER: return "world_map";
        case F_PANEL_IN_APP_ENTER:
        case F_PANEL_UNK1_ENTER:
        case F_PANEL_UNK2_ENTER:
        case F_PANEL_UNK3_ENTER:
        case F_PANEL_UNK4_ENTER:
        case F_PANEL_UNK5_ENTER: return "in_app";
        case F_PANEL_DAILY_REWARD_ENTER: return "daily_reward";
        default: return nullptr;
    }
}

// 统一 UI 状态判定（v0.5.42）：screen 唯一来源，替代 dialog_active 布尔判定。
// 判定链：UIPopupMsg 弹窗 → STATE 状态机（主菜单/世界中）→ 教学暂停 → GAMESTATE 剧情
// → popup 栈顶分派（对话框 dialog_* / 面板 panel_*）→ world。
// 与 data_dialog_content_json 判定链同序（popup 最优先，v0.4.39）。
// 修复（v0.5.42）：不再用数据层计数（UICHOICE/NPCTASKLIST）直接判定——NPC 交互后数据残留
// 而 UI 栈已空时旧 dialog_active 误报 true（真机实测：关闭 NPC 对话框后 screen=world 但 dialog_active=true）。
// 现完全以 popup 栈顶为准：栈顶无面板/对话框 → world，残留计数不产生任何误报。
const char* data_ui_screen() {
    uint16_t state = g_state != nullptr ? *reinterpret_cast<uint16_t*>(g_state) : 0xFFFF;
    // UIPopupMsg 不一定进入 popup 栈（启动确认弹窗即是此类），必须先于主菜单分支识别。
    if (g_base != 0 && g_popup_on != nullptr && *reinterpret_cast<uint8_t*>(g_popup_on))
        return "dialog_popup";
    if (state == 4) {
        // 主菜单：按 popup 栈顶细分（v0.4.18 修复：标题屏/存档选择/职业选择）
        switch (data_popup_top_vma()) {
            case F_PANEL_SAVE_SLOT_ENTER: return "main_menu_save_slot";
            case F_PANEL_CHAR_SELECT_ENTER: return "main_menu_character_select";
            case F_PANEL_DAILY_REWARD_ENTER: return "main_menu_daily_reward";
            case F_PANEL_OPTIONS_ENTER: return "main_menu_options";
            case F_PANEL_SETTINGS_ENTER: return "main_menu_settings";
            default: return "main_menu";
        }
    }
    if (state != 5) return "loading";
    // 药水教学残血：自动完成不暂停（阻断教学暂停，避免卡住 API 操控；tutorial_pause 枚举保留不再返回）
    if (tutorial_state() == 6) tutorial_cancel();
    if (data_story_active()) return "dialog_story";
    // popup 栈顶分派：对话框类（dialog_*）优先于面板类（panel_*）
    uintptr_t top_vma = data_popup_top_vma();
    if (top_vma == 0) return "world";  // 无任何面板/对话框（含数据残留场景）
    switch (top_vma) {
        case F_PANEL_WIPEOUT_ENTER: return "dialog_wipeout";        // 死亡面板
        case F_PANEL_NPC_QUEST_ENTER: return "dialog_quest";        // 任务完成面板
        case F_PANEL_NPC_ENTER: return "dialog_npc";                // NPC 对话
        case F_PANEL_CHOICE_ENTER: return "dialog_choice";          // 选择框（事件驱动）
        case F_PANEL_INPUT_COUNT_ENTER: return "dialog_input_count"; // 数量输入
        case F_PANEL_CHARACTER_INFO_ENTER: return "panel_character_info";
        case F_PANEL_INVENTORY_ENTER: return "panel_inventory";
        case F_PANEL_MERCENARY_ENTER: return "panel_mercenary";
        case F_PANEL_CRAFT_ENTER: return "panel_craft";
        case F_PANEL_NPC_REST_ENTER: return "panel_npc_rest";
        case F_PANEL_NPC_REVIVE_ENTER: return "panel_npc_revive";
        case F_PANEL_OPTIONS_ENTER: return "panel_options";
        case F_PANEL_QUESTS_ENTER: return "panel_quests";
        case F_PANEL_SAVE_SLOT_ENTER: return "panel_save_slot";
        case F_PANEL_CHAR_SELECT_ENTER: return "panel_character_select";
        case F_PANEL_SHORTCUT_ENTER: return "panel_shortcut";
        case F_PANEL_SKILLS_ENTER: return "panel_skills";
        case F_PANEL_SHOP_ENTER: return "panel_shop";
        case F_PANEL_SETTINGS_ENTER: return "panel_settings";
        case F_PANEL_WORLD_MAP_ENTER: return "panel_world_map";
        case F_PANEL_IN_APP_ENTER:
        case F_PANEL_UNK1_ENTER:
        case F_PANEL_UNK2_ENTER:
        case F_PANEL_UNK3_ENTER:
        case F_PANEL_UNK4_ENTER:
        case F_PANEL_UNK5_ENTER: return "panel_in_app";
        case F_PANEL_DAILY_REWARD_ENTER: return "panel_daily_reward";
        default: return "panel_ui_panel";  // 未知栈顶兜底
    }
}

