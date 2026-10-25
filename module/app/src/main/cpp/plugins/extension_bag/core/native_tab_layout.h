#pragma once

#include <cstdint>

#include "contracts/result.h"
#include "contracts/ui.h"

void prepare_c18_native_bag_layout_locked_impl();
bool c18_native_bag_hit_locked(int64_t x, int64_t y, int* out_native_bag);
QolResult c18_native_tab_anchor_layout_locked(void* parent, int index,
                                               qol::contracts::UiRect* out_rect);
