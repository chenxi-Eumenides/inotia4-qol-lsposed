#pragma once

#include <cstdint>

bool extension_bag_grid_hit(int64_t x, int64_t y);
int extension_bag_grid_slot_index(int64_t x, int64_t y,
                                  int64_t origin_x, int64_t origin_y);
void* extension_bag_valid_child(void* root, int slot);
void extension_bag_control_abs_pos(void* button, int64_t* ax, int64_t* ay);
