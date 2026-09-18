// 主菜单「更多游戏」按钮 ExecuteProc 绑定（原 game_ui_settings_injection 的 1-29 行）。
//
// 目标地址来自运行时控件实例（G_MAINMENU_BASE + slot → button → data + CB_EXECUTE_PROC），
// 不是生命周期内稳定的 GOT 槽，故不申报为启动期静态 kPtrSlot；D1 保留 PtrHook 行为并以
// infra/scheduler 主线程帧任务绑定/释放（替代原后台轮询线程），迁移豁免见
// scripts/maintenance/arch_exemptions.json（清除条件：P5/U11 动态 UI 绑定器）。
// P5/U11 目标：`own:ui.main_menu.more_games_entry`（kOwnership）内部按控件实例申请运行期
// ptr-slot lease，记录 slot_addr + expected + replacement + instance_generation。

void* more_games_btn() {
    if (g_base == 0) return nullptr;
    return *reinterpret_cast<void**>(g_base + G_MAINMENU_BASE_VMA + G_MAINMENU_MOREGAMES_SLOT);
}

void more_games_clicked(void* ctrl) {
    (void)ctrl;
    SETTINGS_LOG("more games clicked -> open settings panel");
    // 经面板栈统一打开：记录栈位并复用「关栈顶再开」兜底。
    if (!ui_panels_open("settings")) {
        SETTINGS_LOG("settings panel open failed from more games button");
    }
}

// 绑定/重绑定：仅当主菜单控件实例可解析时安装；句柄记录的槽地址变化或目标被改写时先释放再装。
bool settings_try_bind_more_games() {
    if (g_base == 0) return false;
    void* btn = more_games_btn();
    if (btn == nullptr) return false;
    uint8_t* data = *reinterpret_cast<uint8_t**>(reinterpret_cast<uint8_t*>(btn) + CO_DATA);
    if (data == nullptr) return false;
    void** slot = reinterpret_cast<void**>(data + CB_EXECUTE_PROC);
    if (g_more_games_hook.slot == slot) {
        if (*slot == reinterpret_cast<void*>(&more_games_clicked)) {
            g_more_games_injected = true;
            return true;
        }
        // 目标被游戏改写：旧绑定失效，尽力还原后重装。
        settings_release_more_games();
    } else if (g_more_games_hook.slot != nullptr) {
        // 主菜单重建 → 控件实例/槽地址变化：释放旧绑定（地址可能已失效）。
        settings_release_more_games();
    }
    if (!g_more_games_hook.install_typed(slot, &more_games_clicked)) return false;
    g_more_games_injected = true;
    SETTINGS_LOG("more games ExecuteProc hooked: ctrl=%p orig=%p", btn, g_more_games_hook.orig);
    return true;
}
