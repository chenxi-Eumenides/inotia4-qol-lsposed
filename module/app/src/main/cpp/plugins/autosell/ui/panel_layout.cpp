#include "panel_internal.h"

namespace qol::features::autosell {

void* g_hit_root = nullptr;

// 控件绝对坐标（父链累加，同扩展背包/自定义面板口径）。
void autosell_ctrl_abs(void* ctrl, int64_t* x, int64_t* y) {
    const auto* ui = ui_api();
    if (ui == nullptr || ui->control_absolute_origin == nullptr || x == nullptr || y == nullptr ||
        ui->control_absolute_origin(ctrl, x, y).ok != kQolOk) {
        if (x != nullptr) *x = 0;
        if (y != nullptr) *y = 0;
    }
}

int64_t autosell_screen_width() {
    int64_t logical_w = kBaseW;
    int32_t inset_w = 0;
    int32_t inset_h = 0;
    if (ui_screen_inset_width(&inset_w) && ui_screen_inset_height(&inset_h)) {
        const int64_t screen_w = kBaseW + static_cast<int64_t>(inset_w) * 2;
        const int64_t screen_h = kBaseH + static_cast<int64_t>(inset_h) * 2;
        if (screen_w > 0 && screen_h > 0) logical_w = screen_w * kBaseH / screen_h;
    }
    return logical_w;
}

UiRect autosell_panel_rect() {
    const int64_t logical_w = autosell_screen_width();
    const int64_t x = (logical_w - kPanelW) / 2;
    const int64_t y = (kBaseH - kPanelH) / 2;
    return {x, y, kPanelW, kPanelH};
}

UiRect autosell_mask_rect(const UiRect& panel) {
    // 全屏遮罩：逻辑屏宽 = 面板宽 + 面板左右边距×2（与 settings 同口径）。
    return {0, 0, panel.x * 2 + kPanelW, kBaseH};
}

UiRect autosell_equipment_group(const UiRect& panel) {
    return {panel.x + 24, panel.y + 128, 366, 240};
}

UiRect autosell_back_rect() {
    return {qol::contracts::ui_theme::kNavBoxAbsX, qol::contracts::ui_theme::kNavBoxAbsY,
        qol::contracts::ui_theme::kNavBoxW, qol::contracts::ui_theme::kNavBoxH};
}

UiRect autosell_total_rect() {
    return {autosell_screen_width() - qol::contracts::ui_theme::kNavBoxAbsX - kToggleSize,
        qol::contracts::ui_theme::kNavBoxAbsY, kToggleSize, kToggleSize};
}

UiRect autosell_rule_row(const UiRect& panel, int index) {
    if (index < 3) return {panel.x + 40, panel.y + 184 + index * 52, 334, 44};
    return {panel.x + 433, panel.y + 184 + (index - 3) * 52, 334, 44};
}

UiRect autosell_rule_prev(const UiRect& panel, int index) {
    const UiRect row = autosell_rule_row(panel, index);
    return {row.x + 142, row.y + 1, 38, 42};
}

UiRect autosell_rule_next(const UiRect& panel, int index) {
    const UiRect row = autosell_rule_row(panel, index);
    return {row.x + 292, row.y + 1, 38, 42};
}

bool autosell_hit_test(UiRect area, int64_t x, int64_t y) {
    if (g_hit_root != nullptr) {
        // ui_hit_test 按控件绝对原点 + 传入宽高判定；复用一个无父控件表示不同区域，
        // 不把这些临时区域挂入游戏控件树，也不会触发默认 DrawProc。
        ui_set_rect(g_hit_root, area);
        return ui_hit_test(g_hit_root, x, y, {0, 0, area.w, area.h});
    }
    return x >= area.x && x < area.x + area.w && y >= area.y && y < area.y + area.h;
}

bool autosell_touch_xy(uint64_t param, int64_t* x, int64_t* y) {
    // param 仅在 event 0x17/0x18 中是 {x, y, id} 坐标指针；其它事件不得解引用。
    if (param == 0 || x == nullptr || y == nullptr) return false;
    *x = *reinterpret_cast<const int64_t*>(param);
    *y = *reinterpret_cast<const int64_t*>(param + 8);
    return true;
}

}  // namespace qol::features::autosell
