#pragma once

#include <cstdint>

#include "../../../contracts/host_sdk.h"
#include "../../../contracts/ui.h"

// 设置页自持的绘图/几何 helper（ADR 0001 D6：页面自持薄 helper）。
//
// 这些函数原先混在 plugins/core.custom_ui/ui_components.cpp 中（与组件能力实现同文件），
// 但它们只服务设置页，且自身不碰游戏符号——只转发到宿主适配表 sdk->ui 与本插件的
// 组件能力。按 ADR 0001 的次序，先把它们从框架文件搬到设置页侧，令设置页不再依赖
// 框架的私有头；抽出 core.settings 时这组实现随页面一起走。
//
// 依赖通过 settings_ui_bind 注入（而非直接引用框架文件的静态变量），因此抽出后
// core.settings 只需改为经能力取得同一对指针，调用点不变。
//
// 线程纪律：最终操作游戏控件/绘制，只允许在游戏主线程调用。

namespace qol::features::core_ui {

// 注入宿主 UI 适配表与组件能力实现；两者都必须非空且版本已校验。
void settings_ui_bind(const contracts::QolUiAdapter* adapter,
                      const contracts::QolUiComponents* components);
void settings_ui_unbind();

using UiRect = contracts::UiRect;
using UiClickProc = contracts::QolUiClickFn;
using UiDrawProc = contracts::QolUiDrawFn;

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
void ui_unload_image_unit(int32_t unit);
bool ui_draw_image_part(int32_t unit, int32_t loc, int32_t x, int32_t y, int32_t type, int32_t flip);
bool ui_draw_control_image_part_centered(void* ctrl, int32_t unit, int32_t loc, int32_t type,
                                         int32_t flip);
bool ui_draw_button_background(void* ctrl, UiRect rect, uint32_t color);
bool ui_draw_button_border(void* ctrl, UiRect rect, uint32_t color, int thickness);
bool ui_draw_control(void* ctrl);

// 适配表访问器：设置页大量直接调用（原在框架 ui_components.h 中声明并提供）。
const contracts::QolUiAdapter* ui_adapter();

// 复用原版外观的薄封装（原与 helper 同文件，属页面面向的便利层）。
namespace ui_original {

bool load_option_images();
void unload_option_images();
bool draw_back_button(void* ctrl, bool pressed);
bool draw_toggle(void* ctrl, bool enabled);

}  // namespace ui_original

namespace ui_custom {

void draw_button(void* ctrl, UiRect rect, uint32_t background, uint32_t border, int border_thickness,
                 const char* text, uint32_t text_color, int text_x, int text_y);

}  // namespace ui_custom

}  // namespace qol::features::core_ui
