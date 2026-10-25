#pragma once

#include "contracts/move_merge.h"

bool move_merge_enabled();
bool set_extension_source_protection_enabled(bool enabled);

namespace qol::features::extension_bag {

bool merge_projection_available();
qol::contracts::MoveMergeProjectionDropResult merge_projection_drop(
    void* destination_control, void* source_control);
QolResult merge_projection_identify_extension_item(qol::contracts::QolItemHandle item,
                                                    int32_t* out_bag, int32_t* out_slot);

}  // namespace qol::features::extension_bag
