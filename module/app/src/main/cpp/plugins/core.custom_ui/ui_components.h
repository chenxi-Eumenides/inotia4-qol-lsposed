#pragma once

#include <cstdint>

#include "../../contracts/ui.h"

namespace qol::features::core_ui {

using UiRect = contracts::UiRect;
using UiClickProc = contracts::QolUiClickFn;
using UiDrawProc = contracts::QolUiDrawFn;

// 组件实现只保存宿主注入的 UI 表，不直接依赖游戏内部头文件。
bool ui_components_bind(const contracts::QolUiAdapter* adapter);
void ui_components_unbind();
const contracts::QolUiComponents* ui_components_api();
const contracts::QolUiAdapter* ui_adapter();

// 设置面板的迁移期薄封装：保持原面板的控件句柄和绘制回调签名。
void* ui_create_root(UiRect rect);
void* ui_create_button(void* parent, UiRect rect, const char* text, UiClickProc on_click,
                       UiDrawProc draw_proc);
void* ui_create_label(void* parent, UiRect rect, const char* text, UiDrawProc draw_proc);
void ui_set_rect(void* ctrl, UiRect rect);
bool ui_get_rect(void* ctrl, UiRect* rect);
bool ui_hit_test(void* ctrl, int64_t x, int64_t y, UiRect rect);

void ui_begin_frame();
void ui_fill_rect_alpha(UiRect rect, uint32_t color, uint32_t alpha);
void ui_end_frame();
void ui_draw_panel_decor(UiRect panel, const int64_t* separators, uint32_t count, uint32_t color);
void ui_draw_vertical_line(int64_t x, int64_t y, int64_t h, uint32_t color, int thickness);
void ui_draw_text(void* ctrl, int x, int y, uint32_t color);
void ui_draw_text_centered(void* ctrl, int y, uint32_t color);
bool ui_load_image_unit(int32_t unit);
// 不声明卸载 helper：共享图集 0x0f/0x59/0x4f/0x50 由游戏拥有，模块只确保加载与借用
// 绘制，任何关闭/回滚路径都不得卸载（UnitUnload 无引用计数）。
bool ui_draw_image_part(int32_t unit, int32_t loc, int32_t x, int32_t y, int32_t type, int32_t flip);
bool ui_draw_control_image_part(void* ctrl, int32_t unit, int32_t loc, int32_t type, int32_t flip);
bool ui_draw_control_image_part_centered(void* ctrl, int32_t unit, int32_t loc, int32_t type,
                                         int32_t flip);
bool ui_draw_button_background(void* ctrl, UiRect rect, uint32_t color);
bool ui_draw_button_border(void* ctrl, UiRect rect, uint32_t color, int thickness);
bool ui_draw_control_alpha_overlay(void* ctrl, UiRect rect, uint32_t color, uint32_t alpha);
bool ui_draw_control(void* ctrl);

namespace ui_original {

bool load_option_images();
// 不声明 unload_option_images：共享 option 图集由游戏拥有，页面不卸载。
bool draw_back_button(void* ctrl, bool pressed);
bool draw_toggle(void* ctrl, bool enabled);

}  // namespace ui_original

namespace ui_custom {

void draw_button(void* ctrl, UiRect rect, uint32_t background, uint32_t border, int border_thickness,
                 const char* text, uint32_t text_color, int text_x, int text_y);

}  // namespace ui_custom

}  // namespace qol::features::core_ui
