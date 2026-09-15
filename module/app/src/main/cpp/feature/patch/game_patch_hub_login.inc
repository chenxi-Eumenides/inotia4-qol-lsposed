// 主菜单「开始游戏」按钮：跳过 Com2uS Hive 注册/登录询问弹窗（常驻 patch，启动期应用一次）。
//
// libgame.so 反汇编（monster v27 / 大修 20260830 / 原版 v1.3.2 三变体该区段字节完全一致）：
//   STATE_EnterMainMenu(0x151fac) 里 ControlButton_Create(parent, 0x153024)——0x153024 是主菜单
//   「开始游戏」按钮的 ExecuteProc（未导出局部函数，dynsym 无符号名，紧随
//   STATE_HideModuleMainMenu 0x152ff4 之后）。其流程：
//     0x153034 bl isAuthUser ; 0x153038 uxtb w0,w0 ; 0x15303c cbz w0,0x15305c
//     - 已登录（w0!=0）→ 0x153040 C2S_HubIsRequiredGemPopup 判定（本 patch 不动该分支）
//     - 未登录（w0==0）→ 0x15305c GetCharStringFromPlist(...,"HUB_LOGIN_INFO",lang) +
//       UIPopupMsg_CreateYesNo（是=MainMenu_HubLoginOK / 否=MainMenu_HubLoginCancel）
//   用户点「否」走的 MainMenu_HubLoginCancel(0x1530e4) 就是 UI_SetPopupProcessInfo(1,0) → 存档选择
//   面板（真机实证：点「否」后直接进入存档槽列表）。
// 需求：未登录时不再询问，直接进存档选择。故把 0x15303c 的分支目标从弹窗块 0x15305c 改为
// 「直接进入」块 0x1530a8（同为 UI_SetPopupProcessInfo(1,0)，且返回值同样为 1）。已登录分支不受影响。
// 指令编码：cbz w0,#imm19 = 0x34000000 | (imm19<<5) | Rt（imm19 为相对 PC 的字偏移）
//   原 0x34000100 = cbz w0, 0x15305c（imm19=8）
//   新 0x34000360 = cbz w0, 0x1530a8（imm19=27）
#define P(macro, off, o, r) { #macro, macro, off, o, r }
const PatchEntry g_hub_login_skip_patches[] = {
    P(F_STATE_HIDE_MODULE_MAIN_MENU_VMA, 0x48, 0x34000100, 0x34000360),
};
#undef P
const size_t g_hub_login_skip_patch_count = sizeof(g_hub_login_skip_patches) / sizeof(g_hub_login_skip_patches[0]);
PatchSet g_hub_login_skip_patch_set(g_hub_login_skip_patches, g_hub_login_skip_patch_count);

// 应用常驻 patch（幂等）。失败只记录日志并返回 false，**不阻断 bridge 初始化**：
// 该改动是体验优化，不参与背包/存档等核心链路，匹配失败不应让整个模块失效
// （对比 apply_fixed_stack_layout 的核心链路 fail-closed 策略）。
bool apply_hub_login_popup_skip() {
    std::lock_guard<std::recursive_mutex> lock(g_patch_mtx);
    if (g_base == 0) return false;
    if (g_hub_login_skip_patch_set.applied()) return true;
    const bool ok = g_hub_login_skip_patch_set.apply();
    const PatchEntry& e = g_hub_login_skip_patches[0];
    if (ok) {
        QOL_LOG_INFO(QolDomain::kPlatform, "hub_login_popup_skip macro=%s off=0x%x ok=1",
                     e.func_macro, e.func_offset);
    } else {
        QOL_LOG_ERROR(QolDomain::kPlatform, "hub_login_popup_skip macro=%s off=0x%x ok=0",
                      e.func_macro, e.func_offset);
    }
    return ok;
}
