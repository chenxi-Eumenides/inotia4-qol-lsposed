#pragma once

#include <cstdint>

#include "contracts/ui.h"

bool extension_bag_grid_hit(int64_t x, int64_t y);
int extension_bag_grid_slot_index(int64_t x, int64_t y,
                                  int64_t origin_x, int64_t origin_y);
void* extension_bag_valid_child(void* root, int slot);
void extension_bag_control_abs_pos(void* button, int64_t* ax, int64_t* ay);
bool extension_bag_ui_child_count(void* root, int* out_count);
void* extension_bag_ui_child_at(void* root, int slot);
int extension_bag_ui_user_type(void* control);
void* extension_bag_ui_item(void* control);
void* extension_bag_ui_data(void* control);
int extension_bag_ui_item_slot(void* control);
void* extension_bag_ui_parent(void* control);
void* extension_bag_ui_root(qol::contracts::QolUiRootKind kind);
bool extension_bag_ui_rect(void* control, qol::contracts::UiRect* out_rect);
bool extension_bag_ui_set_rect(void* control, qol::contracts::UiRect rect);
bool extension_bag_ui_control_origin(void* control, int64_t* out_x, int64_t* out_y);
bool extension_bag_ui_hit_test(void* control, int64_t x, int64_t y, qol::contracts::UiRect rect);
bool extension_bag_ui_touch_state(void** out_moving, void** out_drop_source,
                                  int64_t* out_x, int64_t* out_y);
bool extension_bag_ui_touch_coordinates(uint64_t param, int64_t* out_x, int64_t* out_y);
bool extension_bag_ui_draw_part(int32_t unit, int32_t loc, int32_t x, int32_t y,
                                int32_t type, int32_t flip);
bool extension_bag_ui_draw_part_alpha(int32_t unit, int32_t loc, int32_t x, int32_t y,
                                      int32_t type, int32_t flip, int32_t alpha);
bool extension_bag_ui_can_draw();
bool extension_bag_ui_draw_item(void* item, int32_t x, int32_t y, int32_t a, int32_t b);
bool extension_bag_ui_create_control(void* parent, void** out_control);
bool extension_bag_ui_delete_control(void* control);
bool extension_bag_ui_set_control_item(void* control, void* item);
bool extension_bag_ui_set_control_proc(void* control, qol::contracts::QolUiControlProcFn proc);
bool extension_bag_ui_set_control_user_type(void* control, int type);
bool extension_bag_ui_set_control_data(void* control, void* data);
bool extension_bag_ui_allocate_control_data(uint32_t size, void** out_data);
bool extension_bag_ui_unuse_control_event_move(void* control);
bool extension_bag_ui_control_is_moving(void* control);
bool extension_bag_ui_clear_control_drag_flags(void* control);
bool extension_bag_ui_clear_touch_state();
bool extension_bag_ui_set_touch_release_input(int64_t x, int64_t y, uint64_t param);
bool extension_bag_ui_clear_touch_drop_event();
bool extension_bag_ui_fill_rect(qol::contracts::UiRect rect, uint32_t color);
