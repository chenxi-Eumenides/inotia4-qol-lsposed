#pragma once

#include <cstddef>
#include <cstdint>

#include "contracts/item_handle.h"

struct UiRect {
    int64_t x;
    int64_t y;
    int64_t w;
    int64_t h;
};

using UiClickProc = void (*)(void* ctrl);
using UiDrawProc = void (*)(void* ctrl);
using UiControlProc = uint64_t (*)(void*, uint64_t, void*, void*);

void* ui_create_root(UiRect rect);
void* ui_create_button(void* parent, UiRect rect, const char* text,
                       UiClickProc on_click, UiDrawProc draw_proc);
void* ui_create_label(void* parent, UiRect rect, const char* text, UiDrawProc draw_proc);
void ui_set_rect(void* ctrl, UiRect rect);
bool ui_get_rect(void* ctrl, UiRect* rect);
void* ui_find_child_in_area(void* parent, UiRect area);
bool ui_clone_button_style(void* target, void* source, UiClickProc on_click);
bool ui_hit_test(void* ctrl, int64_t x, int64_t y, UiRect rect);
void* ui_original_bag_container();
void* ui_find_root(uint32_t kind);
bool ui_control_type(void* ctrl, int32_t* out_type);
bool ui_control_active(void* ctrl, int32_t* out_active);
bool ui_set_control_show(void* ctrl, bool show);
bool ui_control_user_type(void* ctrl, int32_t* out_type);
void* ui_control_data(void* ctrl);
void* ui_control_item(void* ctrl);
bool ui_control_item_slot(void* ctrl, int32_t* out_slot);
bool ui_control_item_identity(void* ctrl, qol::contracts::QolItemHandle* out_item);
bool ui_item_proc_source_control(void* destination, uint64_t event, const void* param,
                                 void** out_source);
void* ui_control_parent(void* ctrl);
bool ui_execute_proc_matches(void* ctrl, UiClickProc expected);
bool ui_set_button_enabled(void* ctrl, bool enabled);
bool ui_control_absolute_origin(void* ctrl, int64_t* out_x, int64_t* out_y);
bool ui_touch_state(void** out_moving, void** out_drop_source, int64_t* out_x, int64_t* out_y);
bool ui_delete_control(void* ctrl);
bool ui_set_control_item(void* ctrl, void* item);
bool ui_set_control_proc(void* ctrl, UiControlProc proc);
bool ui_set_control_user_type(void* ctrl, int32_t type);
bool ui_set_control_data(void* ctrl, void* data);
void* ui_allocate_control_data(size_t size);
bool ui_set_touch_cursor(void* control, void* cursor);
bool ui_reset_touch_moving();
bool ui_reset_touch_selected();
bool ui_control_is_moving(void* ctrl, int32_t* out_moving);
bool ui_clear_control_drag_flags(void* ctrl);
bool ui_clear_touch_state();
bool ui_set_touch_release_input(int64_t x, int64_t y, uint64_t param);
bool ui_clear_touch_drop_event();
void* ui_main_menu_more_games_button();
void ui_main_menu_observe_screen(const char* screen);
bool ui_main_menu_live_tree_snapshot(void* probe, void** out_original, void** out_parent,
                                    int32_t* out_original_in_tree,
                                    int32_t* out_probe_in_tree);
bool ui_popup_stack_top_matches(void (*enter)());
void ui_clear_control_proc(void* ctrl);

void ui_begin_frame();
void ui_begin_frame(UiRect mask, UiRect panel, uint32_t panel_color);
void ui_fill_rect_alpha(UiRect rect, uint32_t color, uint32_t alpha);
void ui_fill_rect(UiRect rect, uint32_t color);
void ui_end_frame();
void ui_draw_panel_decor(UiRect panel, const int64_t* separator_y, size_t separator_count,
                          uint32_t color);
void ui_draw_vertical_line(int64_t x, int64_t y, int64_t h, uint32_t color, int thickness);
void ui_draw_text(void* ctrl, int x_offset, int y_offset, uint32_t color);
void ui_draw_text_centered(void* ctrl, int y_offset, uint32_t color);
bool ui_load_image_unit(int32_t unit);
void ui_unload_image_unit(int32_t unit);
bool ui_draw_control_image_part(void* ctrl, int32_t unit, int32_t loc, int32_t type, int32_t flip);
bool ui_draw_control_image_part_centered(void* ctrl, int32_t unit, int32_t loc, int32_t type,
                                         int32_t flip);
bool ui_draw_image_part(int32_t unit, int32_t loc, int32_t x, int32_t y, int32_t type, int32_t flip);
bool ui_draw_control_title_image_part(void* ctrl, int32_t loc);
void ui_draw_button_background(void* ctrl, UiRect size, uint32_t color);
void ui_draw_button_border(void* ctrl, UiRect size, uint32_t color, int thickness);
void ui_draw_control_alpha_overlay(void* ctrl, UiRect size, uint32_t color, uint32_t alpha);
bool ui_draw_item_porting(void* item, int32_t x, int32_t y, int32_t a, int32_t b);

// 读取 native 全局裁剪矩形快照 (x,y,w,h)，供同一帧内的「快照→设交集→绘制→恢复」使用。
// x<0 是 native「无父裁剪」哨兵（GRPX_ReSetClip 写全 -1），是成功读取而非读取失败；调用方
// 不得把它当普通矩形求交。GOT 或槽指针不可读、未就绪时返回 false（fail-closed），
// 调用方跳过受保护内容而不是「无裁剪照画」。只在游戏主线程调用，不跨帧持有。
bool ui_clip_snapshot(UiRect* out);

// 原版单按钮文本弹窗（word_id 文案）。内部按 game_symbols 解析创建入口；基址未就绪
// 或符号未解析时静默返回。调用方须在释放扩展锁后调用（锁内零 UI）。
void ui_show_text_popup(int32_t text_id);
