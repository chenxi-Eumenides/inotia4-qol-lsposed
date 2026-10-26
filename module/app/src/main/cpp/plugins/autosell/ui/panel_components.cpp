#include "panel_internal.h"

namespace qol::features::autosell {

void* g_component_root = nullptr;
void* g_orphan_component_root = nullptr;
void* g_back_button = nullptr;
void* g_arrows[kRuleCount][2]{};

static void autosell_component_noop(void*) {}

bool autosell_create_panel_components(const UiRect& panel) {
    if (g_ui_components == nullptr || g_ui_components->panel_create == nullptr ||
        g_ui_components->arrow_button_create == nullptr ||
        g_ui_components->arrow_button_draw == nullptr ||
        g_ui_components->nav_button_create == nullptr ||
        g_ui_components->nav_button_draw == nullptr || g_ui_components->destroy == nullptr) {
        return false;
    }
    const qol::contracts::UiPanelDesc root_desc{{panel.x, panel.y, panel.w, panel.h},
        {0, 0, kBaseW, kBaseH}, kPanelFill565, 0};
    if (g_ui_components->panel_create(&root_desc, nullptr, &g_component_root).ok != kQolOk ||
        g_component_root == nullptr) return false;
    const auto local = [&panel](UiRect rect) {
        rect.x -= panel.x;
        rect.y -= panel.y;
        return rect;
    };
    const UiRect back_rect = autosell_back_rect();
    const qol::contracts::UiNavButtonDesc back_desc{local(back_rect),
        qol::contracts::kUiNavButtonForceBack, nullptr, nullptr};
    if (g_ui_components->nav_button_create(&back_desc, g_component_root,
            &g_back_button).ok != kQolOk || g_back_button == nullptr) return false;
    for (int i = 0; i < kRuleCount; ++i) {
        const UiRect left = local(autosell_rule_prev(panel, i));
        const UiRect right = local(autosell_rule_next(panel, i));
        const qol::contracts::UiArrowButtonDesc left_desc{left, qol::contracts::kUiArrowLeft,
            1, &autosell_component_noop, nullptr};
        const qol::contracts::UiArrowButtonDesc right_desc{right, qol::contracts::kUiArrowRight,
            1, &autosell_component_noop, nullptr};
        if (g_ui_components->arrow_button_create(&left_desc, g_component_root,
                &g_arrows[i][0]).ok != kQolOk ||
            g_ui_components->arrow_button_create(&right_desc, g_component_root,
                &g_arrows[i][1]).ok != kQolOk) return false;
    }
    return true;
}

bool autosell_destroy_panel_components() {
    if (g_orphan_component_root != nullptr) {
        const auto* ui = ui_api();
        if (ui == nullptr || ui->delete_control == nullptr ||
            ui->delete_control(g_orphan_component_root).ok != kQolOk) return false;
        g_orphan_component_root = nullptr;
    }
    if (g_component_root == nullptr) return true;
    void* const native_root = g_component_root;
    if (g_ui_components == nullptr || g_ui_components->destroy == nullptr ||
        g_ui_components->destroy(g_component_root).ok != kQolOk) return false;
    g_component_root = nullptr;
    g_back_button = nullptr;
    std::memset(g_arrows, 0, sizeof(g_arrows));
    const auto* ui = ui_api();
    if (ui == nullptr || ui->delete_control == nullptr ||
        ui->delete_control(native_root).ok != kQolOk) {
        g_orphan_component_root = native_root;
        return false;
    }
    return true;
}

void autosell_draw_component_text(const char* text, int32_t x, int32_t y, uint32_t color,
                                  int32_t align, int32_t font) {
    if (g_ui_components != nullptr && g_component_root != nullptr &&
        g_ui_components->draw_control_text != nullptr) {
        (void)g_ui_components->draw_control_text(g_component_root, text, x, y, color, align, font);
    }
}

void autosell_draw_total_toggle(const UiRect& panel) {
    const auto* ui = ui_api();
    if (g_component_root == nullptr || ui == nullptr || ui->fill_rect == nullptr) return;
    const UiRect rect = autosell_total_rect();
    if (g_ui_components != nullptr && g_ui_components->draw_container != nullptr) {
        (void)g_ui_components->draw_container(rect);
    } else {
        constexpr int64_t edge = 2;
        const uint32_t border_color = g_toggle_pressed ? kGold : 0xFF766A58u;
        const UiRect borders[] = {
            {rect.x, rect.y, rect.w, edge}, {rect.x, rect.y + rect.h - edge, rect.w, edge},
            {rect.x, rect.y, edge, rect.h}, {rect.x + rect.w - edge, rect.y, edge, rect.h}};
        for (UiRect border : borders) (void)ui->fill_rect(border, border_color);
    }
    if (g_toggle_pressed) {
        constexpr int64_t edge = 2;
        const UiRect borders[] = {
            {rect.x, rect.y, rect.w, edge}, {rect.x, rect.y + rect.h - edge, rect.w, edge},
            {rect.x, rect.y, edge, rect.h}, {rect.x + rect.w - edge, rect.y, edge, rect.h}};
        for (UiRect border : borders) (void)ui->fill_rect(border, kGold);
    }
    const char* glyph = g_draft.enabled ? "√" : "X";
    const int32_t glyph_x = static_cast<int32_t>(rect.x - panel.x + rect.w / 2);
    const int32_t glyph_y = static_cast<int32_t>(rect.y - panel.y + (rect.h - 38) / 2);
    const uint32_t glyph_color = g_draft.enabled ? kGold : 0xFFAAAAAAu;
    const int32_t offsets[][2] = {{0, 0}, {-1, 0}, {1, 0}, {0, -1}, {0, 1}};
    for (const auto& offset : offsets) {
        autosell_draw_component_text(glyph, glyph_x + offset[0], glyph_y + offset[1],
                                     glyph_color, 2, 2);
    }
}

}  // namespace qol::features::autosell
