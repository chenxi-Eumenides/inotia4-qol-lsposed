#pragma once

#include "contracts/item_handle.h"

#include <mutex>

namespace qol::features::extension_bag::usecases {

bool move_original_to_extension_slot(
    std::unique_lock<std::mutex>& lock, int src_bag, int src_slot, int dst_bag,
    qol::contracts::QolItemHandle expected_item = qol::contracts::kInvalidItemHandle);
bool move_original_to_extension(std::unique_lock<std::mutex>& lock, int dst_bag,
                                void* moving_control);
bool move_extension_to_original(std::unique_lock<std::mutex>& lock, int src_bag,
                                int src_slot, int target_bag);
bool move_extension_to_extension(int src_bag, int src_slot, int dst_bag, int dst_slot);

}  // namespace qol::features::extension_bag::usecases
