#pragma once

#include "contracts/item_handle.h"
#include "plugins/extension_bag/runtime/reentry_lock.h"
#include "plugins/extension_bag/usecases/result.h"

namespace qol::features::extension_bag::usecases {

bool move_original_to_extension_slot(
    ::qol::extension_bag::ReentryLock& lock, int src_bag, int src_slot, int dst_bag,
    qol::contracts::QolItemHandle expected_item = qol::contracts::kInvalidItemHandle);
bool move_original_to_extension(::qol::extension_bag::ReentryLock& lock, int src_bag,
                                int src_slot, int dst_bag,
                                qol::contracts::QolItemHandle expected_item);
bool move_extension_to_original(::qol::extension_bag::ReentryLock& lock, int src_bag,
                                int src_slot, int target_bag);
bool move_extension_to_extension(int src_bag, int src_slot, int dst_bag, int dst_slot);
MoveResult move_item(int from_bag, int from_slot, int to_bag, int to_slot);

}  // namespace qol::features::extension_bag::usecases
