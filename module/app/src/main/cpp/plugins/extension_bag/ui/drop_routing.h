#pragma once

#include "contracts/item_handle.h"

bool resolve_original_drag_source(void* moving_control, int* out_bag, int* out_slot,
                                  qol::contracts::QolItemHandle* out_expected_item);
