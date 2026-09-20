// 主菜单「更多游戏」按钮 ExecuteProc 绑定（原 game_ui_settings_injection 的 1-29 行）。
//
// 目标地址来自运行时控件实例（G_MAINMENU_BASE + slot → button → data + CB_EXECUTE_PROC），
// 不是生命周期内稳定的 GOT 槽，故不申报为启动期静态 kPtrSlot；D1 保留 PtrHook 行为并以
// infra/scheduler 主线程帧任务绑定/释放（替代原后台轮询线程），迁移豁免见
// scripts/maintenance/arch_exemptions.json（清除条件：P5/U11 动态 UI 绑定器）。
// P5/U11 目标：`own:ui.main_menu.more_games_entry`（kOwnership）内部按控件实例申请运行期
// ptr-slot lease，记录 slot_addr + expected + replacement + instance_generation。

void* more_games_btn() {
    const contracts::QolUiAdapter* ui = ui_adapter();
    if (ui == nullptr || ui->find_main_menu_more_games == nullptr) return nullptr;
    void* button = nullptr;
    return contracts::succeeded(ui->find_main_menu_more_games(&button)) ? button : nullptr;
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
    void* btn = more_games_btn();
    if (btn == nullptr) return false;
    const contracts::QolUiAdapter* ui = ui_adapter();
    if (ui == nullptr || ui->bind_main_menu_more_games == nullptr) return false;
    if (g_more_games_binding_token != 0) {
        int32_t matches = 0;
        if (ui->execute_proc_matches != nullptr &&
            contracts::succeeded(ui->execute_proc_matches(btn, &more_games_clicked, &matches)) &&
            matches != 0) {
            g_more_games_injected = true;
            return true;
        }
        settings_release_more_games();
    }
    uint64_t token = 0;
    if (!contracts::succeeded(ui->bind_main_menu_more_games(&more_games_clicked, &token)) ||
        token == 0) return false;
    g_more_games_binding_token = token;
    g_more_games_injected = true;
    SETTINGS_LOG("more games ExecuteProc bound: ctrl=%p", btn);
    return true;
}
