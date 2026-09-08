std::string data_op_main_menu() {
    if (fn_gamestate_set_state == nullptr) return op_err("symbol not resolved");
    if (!extension_bag_prepare_main_menu()) {
        return op_err("extension bag cleanup failed; main menu transition blocked");
    }
    fn_gamestate_set_state(4);
    return op_ok();
}
std::string data_op_panel_close() {
    if (!game_in_world()) return op_err("not in game");
    if (fn_ui_set_popup_process_info == nullptr) return op_err("symbol not resolved");
    // 前置：栈顶必须是面板（enter 匹配 PANELS），非空弹窗栈或弹窗（G_POPUP_ON）不处理
    if (g_base == 0 || g_popup_stack == nullptr) return op_err("libgame not ready");
    uint8_t* stk = reinterpret_cast<uint8_t*>(g_popup_stack);
    uint32_t count = *reinterpret_cast<uint32_t*>(stk + 8);
    if (count == 0 || count > 27) return op_err("no panel open");
    uint64_t data = *reinterpret_cast<uint64_t*>(stk + 0x18);
    if (data == 0) return op_err("no panel open");
    uint8_t* top = reinterpret_cast<uint8_t*>(static_cast<uintptr_t>(data)) + (count - 1) * 0x40;
    uintptr_t enter = *reinterpret_cast<uintptr_t*>(top + 0x10);
    uintptr_t vma = enter > g_base ? enter - g_base : 0;
    if (extension_bag_is_inventory_enter(enter)) vma = F_PANEL_INVENTORY_ENTER;
    bool is_panel = false;
    switch (vma) {
        case F_PANEL_CHARACTER_INFO_ENTER: case F_PANEL_CHOICE_ENTER: case F_PANEL_INVENTORY_ENTER: case F_PANEL_INPUT_COUNT_ENTER:
        case F_PANEL_MERCENARY_ENTER: case F_PANEL_CRAFT_ENTER: case F_PANEL_NPC_ENTER: case F_PANEL_NPC_QUEST_ENTER:
        case F_PANEL_NPC_REST_ENTER: case F_PANEL_NPC_REVIVE_ENTER: case F_PANEL_OPTIONS_ENTER: case F_PANEL_QUESTS_ENTER:
        case F_PANEL_SAVE_SLOT_ENTER: case F_PANEL_CHAR_SELECT_ENTER: case F_PANEL_SHORTCUT_ENTER: case F_PANEL_SKILLS_ENTER:
        case F_PANEL_SHOP_ENTER: case F_PANEL_SETTINGS_ENTER: case F_PANEL_WIPEOUT_ENTER: case F_PANEL_WORLD_MAP_ENTER:
        case F_PANEL_IN_APP_ENTER: case F_PANEL_DAILY_REWARD_ENTER:
        case F_PANEL_UNK1_ENTER: case F_PANEL_UNK2_ENTER: case F_PANEL_UNK3_ENTER:
        case F_PANEL_UNK4_ENTER: case F_PANEL_UNK5_ENTER:
            is_panel = true;
            break;
        default: break;
    }
    if (!is_panel) return op_err("top of stack is not a panel");
    // 官方 ButtonBackExe 链：SOUNDSYSTEM_Play(0) + 流程3 + HUD 开关恢复
    fn_ui_set_popup_process_info(3, 0);
    uint8_t** hud_gate = reinterpret_cast<uint8_t**>(g_base + G_HUD_GATE_GOT_VMA);
    if (hud_gate != nullptr && *hud_gate != nullptr) **hud_gate = 1;
    return op_ok();
}
std::string data_op_panel_open(const std::string& panel) {
    if (!game_in_world()) return op_err("not in game");
    if (fn_ui_set_popup_process_info == nullptr) return op_err("symbol not resolved");
    if (g_base == 0) return op_err("libgame not ready");
    // v0.5.42：兼容裸名与 panel_ 前缀（screen 输出 panel_inventory，输入可两者皆可）
    std::string p = panel;
    if (p.rfind("panel_", 0) == 0) p = p.substr(6);
    // 面板名 → enter VMA（与 data_ui_screen 的 PANELS 映射一致）
    uintptr_t target = 0;
    if (p == "character_info") target = F_PANEL_CHARACTER_INFO_ENTER;
    else if (p == "choice") target = F_PANEL_CHOICE_ENTER;
    else if (p == "inventory") target = F_PANEL_INVENTORY_ENTER;
    else if (p == "input_count") target = F_PANEL_INPUT_COUNT_ENTER;
    else if (p == "mercenary") target = F_PANEL_MERCENARY_ENTER;
    else if (p == "craft") target = F_PANEL_CRAFT_ENTER;
    else if (p == "npc") target = F_PANEL_NPC_ENTER;
    else if (p == "npc_quest") target = F_PANEL_NPC_QUEST_ENTER;
    else if (p == "npc_rest") target = F_PANEL_NPC_REST_ENTER;
    else if (p == "npc_revive") target = F_PANEL_NPC_REVIVE_ENTER;
    else if (p == "options") target = F_PANEL_OPTIONS_ENTER;
    else if (p == "quests") target = F_PANEL_QUESTS_ENTER;
    else if (p == "save_slot") target = F_PANEL_SAVE_SLOT_ENTER;
    else if (p == "character_select") target = F_PANEL_CHAR_SELECT_ENTER;
    else if (p == "shortcut") target = F_PANEL_SHORTCUT_ENTER;
    else if (p == "skills") target = F_PANEL_SKILLS_ENTER;
    else if (p == "shop") target = F_PANEL_SHOP_ENTER;
    else if (p == "settings") target = F_PANEL_SETTINGS_ENTER;
    else if (p == "wipeout") target = F_PANEL_WIPEOUT_ENTER;
    else if (p == "world_map") target = F_PANEL_WORLD_MAP_ENTER;
    else if (p == "in_app") target = F_PANEL_IN_APP_ENTER;
    else if (p == "daily_reward") target = F_PANEL_DAILY_REWARD_ENTER;
    else return op_err("unknown panel");
    // 面板可开白名单（v0.4.34 真机实测收紧）：仅允许不依赖外部上下文的独立面板。
    // 崩溃记录（全部 SIGSEGV，tombstone 已验证）：
    //   options      → GAMELOADER_DrawBackGround→GRPX_DrawPart（主菜单/GAMELOADER 场景专属）
    //   craft/shop   → CHAR_GetName 空指针（需 NPC 交互对象 [0x2f6000+0xc20]→[x0] 就绪）
    //   input_count  → ControlObject_GetActive 空控件（需 inventory 物品数量输入上下文）
    // 语义不正确的面板（v0.4.34 移除）：
    //   choice       → 游戏内由事件/剧情驱动的选择框，API 打开语义不符
    //   world_map    → 由游戏内事件（如保存点）驱动的世界地图，API 打开语义不符
    //   wipeout      → 角色死亡时游戏自动打开，非用户可操作面板
    // 其余未实证面板（npc 系列/shortcut/in_app 等）同样拒绝，避免 API 直接 Push 崩溃。
    bool openable = (p == "character_info" || p == "inventory" ||
                     p == "mercenary" || p == "quests" || p == "settings" ||
                     p == "skills");
    if (!openable) return op_err("panel requires in-game context");
    // 扫描 state list 找 enter == g_base+target 的 state id
    uint8_t* list = *reinterpret_cast<uint8_t**>(g_base + G_POPUP_STATE_LIST_GOT_VMA);
    if (list == nullptr) return op_err("state list not ready");
    int state_id = p == "inventory" ? extension_bag_inventory_state_id() : -1;
    for (int i = 0; state_id < 0 && i < 27; ++i) {
        uintptr_t enter = *reinterpret_cast<uintptr_t*>(list + i * 0x40 + 0x10);
        if (enter == g_base + target) { state_id = i; break; }
    }
    if (state_id < 0) return op_err("panel state not found");

    // UI_SetPopupProcessInfo(1, id) 是 Push，不会替换已有栈顶。相同面板直接幂等返回；
    // 切换面板先走官方 Pop，并等待主循环真正清空栈，避免 Pop/Open 同帧再次叠加。
    uintptr_t top_vma = data_popup_top_vma();
    if (top_vma != 0) {
        if (top_vma == target) return op_ok();
        bool top_is_panel = false;
        switch (top_vma) {
            case F_PANEL_CHARACTER_INFO_ENTER: case F_PANEL_INVENTORY_ENTER:
            case F_PANEL_MERCENARY_ENTER: case F_PANEL_QUESTS_ENTER:
            case F_PANEL_CRAFT_ENTER: case F_PANEL_NPC_REST_ENTER:
            case F_PANEL_NPC_REVIVE_ENTER: case F_PANEL_OPTIONS_ENTER:
            case F_PANEL_SAVE_SLOT_ENTER: case F_PANEL_CHAR_SELECT_ENTER:
            case F_PANEL_SHORTCUT_ENTER: case F_PANEL_SKILLS_ENTER:
            case F_PANEL_SHOP_ENTER: case F_PANEL_SETTINGS_ENTER:
            case F_PANEL_WORLD_MAP_ENTER: case F_PANEL_IN_APP_ENTER:
            case F_PANEL_DAILY_REWARD_ENTER:
            case F_PANEL_UNK1_ENTER: case F_PANEL_UNK2_ENTER:
            case F_PANEL_UNK3_ENTER: case F_PANEL_UNK4_ENTER:
            case F_PANEL_UNK5_ENTER:
                top_is_panel = true;
                break;
            default: break;
        }
        if (!top_is_panel) return op_err("top of stack is not a panel");
        fn_ui_set_popup_process_info(3, 0);
        if (!wait_for_popup_stack_empty()) return op_err("panel close pending");
    }
    fn_ui_set_popup_process_info(1, state_id);
    return op_ok();
}
