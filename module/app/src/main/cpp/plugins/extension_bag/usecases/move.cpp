#include "plugins/extension_bag/usecases/move.h"

#include "plugins/extension_bag/transaction/transaction.h"

namespace qol::features::extension_bag::usecases {

bool move_original_to_extension_slot(
    std::unique_lock<std::mutex>& lock, int src_bag, int src_slot, int dst_bag,
    qol::contracts::QolItemHandle expected_item) {
    return ::move_original_to_extension_slot_locked(lock, src_bag, src_slot, dst_bag,
                                                    expected_item);
}

bool move_original_to_extension(std::unique_lock<std::mutex>& lock, int dst_bag,
                                void* moving_control) {
    return ::move_original_to_extension_locked(lock, dst_bag, moving_control);
}

bool move_extension_to_original(std::unique_lock<std::mutex>& lock, int src_bag,
                                int src_slot, int target_bag) {
    return ::move_extension_to_original_locked(lock, src_bag, src_slot, target_bag);
}

bool move_extension_to_extension(int src_bag, int src_slot, int dst_bag, int dst_slot) {
    return ::move_extension_to_extension_locked(src_bag, src_slot, dst_bag, dst_slot);
}

}  // namespace qol::features::extension_bag::usecases
