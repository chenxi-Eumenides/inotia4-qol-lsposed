void* more_games_btn() {
    if (g_base == 0) return nullptr;
    return *reinterpret_cast<void**>(g_base + G_MAINMENU_BASE_VMA + G_MAINMENU_MOREGAMES_SLOT);
}

void more_games_clicked(void* ctrl) {
    SETTINGS_LOG("more games clicked -> open settings panel");
    if (fn_ui_set_popup_process_info == nullptr || g_state_id < 0) return;
    fn_ui_set_popup_process_info(1, g_state_id);
}

bool inject_more_games_btn() {
    std::lock_guard<std::mutex> lock(g_settings_mtx);
    if (g_more_games_injected) return true;
    if (g_base == 0) return false;
    void* btn = more_games_btn();
    if (btn == nullptr) return false;
    uint8_t* data = *reinterpret_cast<uint8_t**>(reinterpret_cast<uint8_t*>(btn) + CO_DATA);
    if (data == nullptr) return false;
    void** slot = reinterpret_cast<void**>(data + CB_EXECUTE_PROC);
    if (*slot == reinterpret_cast<void*>(&more_games_clicked)) {
        g_more_games_injected = true;
        return true;
    }
    if (!g_more_games_hook.install_typed(slot, &more_games_clicked)) return false;
    g_more_games_injected = true;
    SETTINGS_LOG("more games ExecuteProc hooked: ctrl=%p orig=%p", btn, g_more_games_hook.orig);
    return true;
}

// ---- 主菜单「更多游戏」绘制层文字覆盖 ----
// 原版主菜单按钮文字是 GetGroupTitleImgType 贴图的一部分：STATE_DrawMainMenu 在
// GRPX_Start 后逐个取按钮 ControlObject_GetAbsoluteRect 再 GRPX_DrawPart 绘制。
// 这里在整段主菜单绘制完成后，按同一绝对矩形口径覆盖该控件可见区域并画中文标题；
// 控件、Rect、Data、ExecuteProc 与触摸链均不改动（命中仍走原控件树）。
// 覆盖层配色沿用设置面板同系深底 + 金色描边/文字。
constexpr uint32_t kMoreGamesOverlayBg = 0xFF201810;        // ABGR 深棕底
constexpr uint32_t kMoreGamesOverlayBgPressed = 0xFF33261C; // 按下态略亮
constexpr uint32_t kMoreGamesOverlayBorder = 0xFF00B4D7;    // ABGR 金
constexpr int32_t kMoreGamesOverlayTextR = 0xD7;
constexpr int32_t kMoreGamesOverlayTextG = 0xB4;
constexpr int32_t kMoreGamesOverlayTextB = 0x00;
constexpr int32_t kMoreGamesOverlayTextH = 0x18;            // 文字行高（居中偏移用）
char kMoreGamesOverlayText[] = "QoL模块设置";

void draw_more_games_overlay() {
    if (g_base == 0) return;
    void* btn = more_games_btn();
    if (btn == nullptr) return;
    if (fn_grpx_fill_rect == nullptr || fn_grpx_set_font_color_rgb == nullptr ||
        fn_grpx_draw_string_with_font == nullptr) {
        return;
    }
    uint8_t* ctrl = static_cast<uint8_t*>(btn);
    const int64_t w = *reinterpret_cast<int64_t*>(ctrl + CO_RECT_W);
    const int64_t h = *reinterpret_cast<int64_t*>(ctrl + CO_RECT_H);
    if (w <= 0 || h <= 0) return;
    uint8_t* data = *reinterpret_cast<uint8_t**>(ctrl + CO_DATA);
    const bool pressed = data != nullptr &&
                         *reinterpret_cast<uint8_t*>(data + CB_STATE) == 1;
    const UiRect box = {0, 0, w, h};
    // 覆盖原「更多游戏」贴图区（含原文字），避免残留；再居中写模块标题。
    ui_draw_button_background(btn, box, pressed ? kMoreGamesOverlayBgPressed : kMoreGamesOverlayBg);
    ui_draw_button_border(btn, box, kMoreGamesOverlayBorder, 2);
    // 沿父链累加绝对矩形（与 ui_draw_* 同口径；ControlObject_GetAbsoluteRect 用 x8
    // sret 出参，C++ 不能直调，故手工父链读），取水平中心。
    int64_t x = 0;
    int64_t y = 0;
    for (void* current = btn; current != nullptr;) {
        uint8_t* c = static_cast<uint8_t*>(current);
        x += *reinterpret_cast<int64_t*>(c + CO_RECT_X);
        y += *reinterpret_cast<int64_t*>(c + CO_RECT_Y);
        current = *reinterpret_cast<void**>(c + CO_PARENT);
    }
    fn_grpx_set_font_color_rgb(kMoreGamesOverlayTextR, kMoreGamesOverlayTextG,
                               kMoreGamesOverlayTextB);
    fn_grpx_draw_string_with_font(kMoreGamesOverlayText,
                                  static_cast<int32_t>(x + w / 2),
                                  static_cast<int32_t>(y + (h - kMoreGamesOverlayTextH) / 2),
                                  2, 1);
}

// 复刻原 `bl STATE_DrawMainMenu`，随后覆盖「更多游戏」可见文字。
void state_draw_main_menu_wrapper() {
    if (fn_state_draw_main_menu != nullptr) fn_state_draw_main_menu();
    draw_more_games_overlay();
}

// 安装 STATE_ProcessMainMenu 内 `bl STATE_DrawMainMenu` 的调用点 patch（core call_patch，
// fail-closed：指令字不符即拒装）。与设置面板/「更多游戏」点击 hook 互不依赖，
// 安装失败只保持原版绘制；已安装时幂等返回，避免重复改码。
bool install_more_games_overlay_if_ready() {
    if (g_mainmenu_overlay_installed) return true;
    if (g_base == 0 || !bridge_ready()) return false;
    fn_state_draw_main_menu = reinterpret_cast<StateDrawMainMenuFn>(
        g_base + fn_resolve("F_STATE_DRAW_MAIN_MENU_VMA", F_STATE_DRAW_MAIN_MENU_VMA));
    if (fn_state_draw_main_menu == nullptr) return false;
    const uintptr_t call_addr =
        g_base + fn_resolve("F_STATE_PROCESS_MAIN_MENU_VMA", F_STATE_PROCESS_MAIN_MENU_VMA) +
        F_STATE_PROCESS_MAIN_MENU_DRAW_CALL_OFF;
    if (!call_patch_install_bl(call_addr, STATE_PROCESS_MAIN_MENU_DRAW_BL_WORD,
                               reinterpret_cast<void*>(&state_draw_main_menu_wrapper))) {
        SETTINGS_LOG("main menu draw overlay patch failed call=%p",
                     reinterpret_cast<void*>(call_addr));
        return false;
    }
    g_mainmenu_overlay_installed = true;
    SETTINGS_LOG("main menu draw overlay patch installed: more games -> QoL module settings");
    return true;
}

void ensure_inject_thread() {
    if (g_thread_started.exchange(true)) return;
    std::thread([]() {
        for (;;) {
            const char* screen = data_ui_screen();
            bool on_main_menu = screen != nullptr && strcmp(screen, "main_menu") == 0;
            if (on_main_menu) {
                inject_state_entry_locked();
                inject_more_games_btn();
            } else {
                std::lock_guard<std::mutex> lock(g_settings_mtx);
                // 离开主菜单（v0.7.x）：还原 PopupState 回调与按钮 hook，
                // 不永久占用 IAP 死条目 / 更多游戏按钮，与 savebackup UI 同策略。
                if (g_state_entry != nullptr) {
                    memcpy(g_state_entry, g_state_backup, POPUP_STATE_SIZE);
                    g_state_entry = nullptr;
                    g_state_id = -1;
                }
                if (g_more_games_injected) {
                    g_more_games_injected = false;
                    g_more_games_hook.slot = nullptr;
                    g_more_games_hook.orig = nullptr;
                }
                g_panel_active = false;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(300));
        }
    }).detach();
}

// ---- state list 条目定位与注入 ----

uint8_t* find_state_entry(uintptr_t enter_vma) {
    if (g_base == 0) return nullptr;
    void** got = reinterpret_cast<void**>(g_base + G_POPUP_STATE_LIST_GOT_VMA);
    if (got == nullptr || *got == nullptr) return nullptr;
    uint8_t* list = reinterpret_cast<uint8_t*>(*got);
    for (int i = 0; i < 27; ++i) {
        uintptr_t enter = *reinterpret_cast<uintptr_t*>(list + i * POPUP_STATE_SIZE + POPUP_ENTRY_ENTER);
        if (enter == g_base + enter_vma) return list + i * POPUP_STATE_SIZE;
    }
    return nullptr;
}

bool inject_state_entry_locked() {
    std::lock_guard<std::mutex> lock(g_settings_mtx);
    if (g_state_entry != nullptr) return true;
    if (g_base == 0) return false;
    uint8_t* entry = find_state_entry(F_PANEL_UNK1_ENTER);
    if (entry == nullptr) {
        SETTINGS_LOG("state entry (INAP_GEMSHOP) not found");
        return false;
    }
    memcpy(g_state_backup, entry, POPUP_STATE_SIZE);
    *reinterpret_cast<uintptr_t*>(entry + POPUP_ENTRY_ENTER) = reinterpret_cast<uintptr_t>(&settings_panel_enter);
    *reinterpret_cast<uintptr_t*>(entry + POPUP_ENTRY_PROCESS) = reinterpret_cast<uintptr_t>(&settings_panel_process);
    *reinterpret_cast<uintptr_t*>(entry + POPUP_ENTRY_F3) = reinterpret_cast<uintptr_t>(&settings_panel_f3);
    *reinterpret_cast<uintptr_t*>(entry + POPUP_ENTRY_F4) = reinterpret_cast<uintptr_t>(&settings_panel_f4);
    *reinterpret_cast<uintptr_t*>(entry + POPUP_ENTRY_EVENT) = reinterpret_cast<uintptr_t>(&settings_panel_event);
    g_state_entry = entry;
    g_state_id = *reinterpret_cast<int32_t*>(entry);
    SETTINGS_LOG("state entry %d injected (enter/process/f3/f4/event)", g_state_id);
    return true;
}

