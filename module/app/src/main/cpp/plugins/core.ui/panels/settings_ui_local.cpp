#include "settings_ui_local.h"

// 设置页自持 helper 实现（ADR 0001 D6）。原实现位于 ui_components.cpp，与组件能力
// 实现同文件；此处只做搬运 + 依赖注入，行为不变：
//   · 创建/设置几何 → 组件能力（contracts::QolUiComponents）
//   · 绘制/帧/图像/命中测试 → 宿主适配表（contracts::QolUiAdapter，即 sdk->ui）
// 原先 ui_get_rect/ui_hit_test 依赖框架内部组件注册表；改走适配表 get_rect/hit_test，
// 使本模块不再引用框架私有状态，可随页面整体抽出。

namespace qol::features::core_ui {
namespace {

const contracts::QolUiAdapter* g_page_adapter = nullptr;
const contracts::QolUiComponents* g_page_components = nullptr;

}  // namespace

void settings_ui_bind(const contracts::QolUiAdapter* adapter,
                      const contracts::QolUiComponents* components) {
    g_page_adapter = adapter;
    g_page_components = components;
}

void settings_ui_unbind() {
    g_page_adapter = nullptr;
    g_page_components = nullptr;
}

void* ui_create_root(UiRect rect) {
    if (g_page_components == nullptr || g_page_components->panel_create == nullptr) return nullptr;
    const contracts::UiPanelDesc desc{rect, rect, 0, 0};
    contracts::QolUiHandle out = nullptr;
    const ::QolResult result = g_page_components->panel_create(&desc, nullptr, &out);
    return contracts::succeeded(result) ? out : nullptr;
}

void* ui_create_button(void* parent, UiRect rect, const char* text, UiClickProc on_click,
                       UiDrawProc draw_proc) {
    if (g_page_components == nullptr || g_page_components->button_create == nullptr) return nullptr;
    contracts::UiButtonDesc desc{rect, text, 0, on_click, nullptr};
    contracts::QolUiHandle out = nullptr;
    const ::QolResult result = g_page_components->button_create(&desc, parent, &out);
    if (!contracts::succeeded(result)) return nullptr;
    if (on_click != nullptr && g_page_adapter != nullptr && g_page_adapter->set_click_proc != nullptr) {
        const ::QolResult click_result = g_page_adapter->set_click_proc(out, on_click, out);
        if (!contracts::succeeded(click_result)) return nullptr;
    }
    if (draw_proc != nullptr && g_page_adapter != nullptr && g_page_adapter->set_draw_proc != nullptr) {
        const ::QolResult draw_result = g_page_adapter->set_draw_proc(out, draw_proc);
        if (!contracts::succeeded(draw_result)) return nullptr;
    }
    return out;
}

void* ui_create_label(void* parent, UiRect rect, const char* text, UiDrawProc draw_proc) {
    if (g_page_components == nullptr || g_page_components->label_create == nullptr) return nullptr;
    contracts::UiLabelDesc desc{rect, text, 0, contracts::UiAlign::kLeft};
    contracts::QolUiHandle out = nullptr;
    const ::QolResult result = g_page_components->label_create(&desc, parent, &out);
    if (!contracts::succeeded(result)) return nullptr;
    if (draw_proc != nullptr && g_page_adapter != nullptr && g_page_adapter->set_draw_proc != nullptr) {
        const ::QolResult draw_result = g_page_adapter->set_draw_proc(out, draw_proc);
        if (!contracts::succeeded(draw_result)) return nullptr;
    }
    return out;
}

void ui_set_rect(void* ctrl, UiRect rect) {
    if (g_page_components == nullptr || g_page_components->set_rect == nullptr) return;
    (void)g_page_components->set_rect(ctrl, rect);
}

bool ui_get_rect(void* ctrl, UiRect* rect) {
    if (ctrl == nullptr || rect == nullptr) return false;
    if (g_page_adapter == nullptr || g_page_adapter->get_rect == nullptr) return false;
    return contracts::succeeded(g_page_adapter->get_rect(ctrl, rect));
}

bool ui_hit_test(void* ctrl, int64_t x, int64_t y, UiRect rect) {
    if (g_page_adapter == nullptr || g_page_adapter->hit_test == nullptr) return false;
    return g_page_adapter->hit_test(ctrl, x, y, rect) > 0;
}

void ui_begin_frame() {
    if (g_page_adapter != nullptr && g_page_adapter->begin_frame != nullptr) {
        (void)g_page_adapter->begin_frame();
    }
}

void ui_fill_rect_alpha(UiRect rect, uint32_t color, uint32_t alpha) {
    if (g_page_adapter != nullptr && g_page_adapter->fill_rect_alpha != nullptr) {
        (void)g_page_adapter->fill_rect_alpha(rect, color, alpha);
    }
}

void ui_end_frame() {
    if (g_page_adapter != nullptr && g_page_adapter->end_frame != nullptr) {
        (void)g_page_adapter->end_frame();
    }
}

void ui_draw_panel_decor(UiRect panel, const int64_t* separators, uint32_t count, uint32_t color) {
    if (g_page_adapter != nullptr && g_page_adapter->draw_panel_decor != nullptr) {
        (void)g_page_adapter->draw_panel_decor(panel, separators, count, color);
    }
}

void ui_draw_vertical_line(int64_t x, int64_t y, int64_t h, uint32_t color, int thickness) {
    if (g_page_adapter != nullptr && g_page_adapter->draw_vertical_line != nullptr) {
        (void)g_page_adapter->draw_vertical_line(x, y, h, color, thickness);
    }
}

void ui_draw_text(void* ctrl, int x, int y, uint32_t color) {
    if (g_page_adapter != nullptr && g_page_adapter->draw_text != nullptr) {
        (void)g_page_adapter->draw_text(ctrl, x, y, color);
    }
}

void ui_draw_text_centered(void* ctrl, int y, uint32_t color) {
    if (g_page_adapter != nullptr && g_page_adapter->draw_text_centered != nullptr) {
        (void)g_page_adapter->draw_text_centered(ctrl, y, color);
    }
}

bool ui_load_image_unit(int32_t unit) {
    return g_page_adapter != nullptr && g_page_adapter->load_image_unit != nullptr &&
        contracts::succeeded(g_page_adapter->load_image_unit(unit));
}

void ui_unload_image_unit(int32_t unit) {
    if (g_page_adapter != nullptr && g_page_adapter->unload_image_unit != nullptr) {
        (void)g_page_adapter->unload_image_unit(unit);
    }
}

bool ui_draw_image_part(int32_t unit, int32_t loc, int32_t x, int32_t y, int32_t type, int32_t flip) {
    return g_page_adapter != nullptr && g_page_adapter->draw_image_part != nullptr &&
        contracts::succeeded(g_page_adapter->draw_image_part(unit, loc, x, y, type, flip));
}

bool ui_draw_control_image_part_centered(void* ctrl, int32_t unit, int32_t loc, int32_t type,
                                         int32_t flip) {
    return g_page_adapter != nullptr && g_page_adapter->draw_control_image_part_centered != nullptr &&
        contracts::succeeded(
            g_page_adapter->draw_control_image_part_centered(ctrl, unit, loc, type, flip));
}

bool ui_draw_button_background(void* ctrl, UiRect rect, uint32_t color) {
    return g_page_adapter != nullptr && g_page_adapter->draw_button_background != nullptr &&
        contracts::succeeded(g_page_adapter->draw_button_background(ctrl, rect, color));
}

bool ui_draw_button_border(void* ctrl, UiRect rect, uint32_t color, int thickness) {
    return g_page_adapter != nullptr && g_page_adapter->draw_button_border != nullptr &&
        contracts::succeeded(g_page_adapter->draw_button_border(ctrl, rect, color, thickness));
}

bool ui_draw_control_alpha_overlay(void* ctrl, UiRect rect, uint32_t color, uint32_t alpha) {
    return g_page_adapter != nullptr && g_page_adapter->draw_control_alpha_overlay != nullptr &&
        contracts::succeeded(g_page_adapter->draw_control_alpha_overlay(ctrl, rect, color, alpha));
}

bool ui_draw_control(void* ctrl) {
    return g_page_adapter != nullptr && g_page_adapter->draw_control != nullptr &&
        contracts::succeeded(g_page_adapter->draw_control(ctrl));
}

const contracts::QolUiAdapter* ui_adapter() { return g_page_adapter; }

namespace ui_original {

bool load_option_images() { return ui_load_image_unit(0x59); }

void unload_option_images() { ui_unload_image_unit(0x59); }

bool draw_back_button(void* ctrl, bool pressed) {
    if (!ui_draw_control_image_part_centered(ctrl, 0x59, 0x3, 2, 1)) return false;
    return !pressed || ui_draw_control_image_part_centered(ctrl, 0x59, 0x2, 2, 1);
}

bool draw_toggle(void* ctrl, bool enabled) {
    const int32_t loc = enabled ? 0x0f : 0x10;
    if (!ui_draw_control_image_part_centered(ctrl, 0x0f, loc, 2, 1)) return false;
    return enabled || ui_draw_control_image_part_centered(ctrl, 0x0f, 0x9, 2, 1);
}

}  // namespace ui_original

namespace ui_custom {

void draw_button(void* ctrl, UiRect rect, uint32_t background, uint32_t border, int border_thickness,
                 const char* text, uint32_t text_color, int text_x, int text_y) {
    (void)ui_draw_button_background(ctrl, rect, background);
    (void)ui_draw_button_border(ctrl, rect, border, border_thickness);
    if (text != nullptr) ui_draw_text(ctrl, text_x, text_y, text_color);
}

}  // namespace ui_custom

}  // namespace qol::features::core_ui
