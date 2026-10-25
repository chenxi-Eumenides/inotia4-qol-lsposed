#include "plugins/extension_bag/usecases/move.h"
#include "plugins/extension_bag/runtime/reentry_lock.h"

#include "plugins/extension_bag/extension_bag_context.h"
#include "plugins/extension_bag/runtime/entry_guard.h"
#include "plugins/extension_bag/runtime/session.h"
#include "plugins/extension_bag/runtime/shared.h"
#include "plugins/extension_bag/transaction/transaction.h"

namespace qol::features::extension_bag::usecases {

bool move_original_to_extension_slot(
    ::qol::extension_bag::ReentryLock& lock, int src_bag, int src_slot, int dst_bag,
    qol::contracts::QolItemHandle expected_item) {
    return ::move_original_to_extension_slot_locked(lock, src_bag, src_slot, dst_bag,
                                                    expected_item);
}

bool move_original_to_extension(::qol::extension_bag::ReentryLock& lock, int dst_bag,
                                void* moving_control) {
    return ::move_original_to_extension_locked(lock, dst_bag, moving_control);
}

bool move_extension_to_original(::qol::extension_bag::ReentryLock& lock, int src_bag,
                                int src_slot, int target_bag) {
    return ::move_extension_to_original_locked(lock, src_bag, src_slot, target_bag);
}

bool move_extension_to_extension(int src_bag, int src_slot, int dst_bag, int dst_slot) {
    return ::move_extension_to_extension_locked(src_bag, src_slot, dst_bag, dst_slot);
}

std::string move_item(int from_bag, int from_slot, int to_bag, int to_slot) {
    if (!qol::extension_bag::extension_bag_on_main_thread()) {
        return extension_bag_not_ready_error_locked_bridge();
    }
    if (!extension_bag_ready_locked_bridge()) return extension_bag_not_ready_error_locked_bridge();
    if (runtime_state().objects.module_view_installed) {
        return op_err("extension view open; movement disabled (P2)");
    }
    if (from_bag == virtual_bag::kOriginalTaskBag ||
        to_bag == virtual_bag::kOriginalTaskBag) {
        return op_err("task bag excluded");
    }
    if (from_slot < 0 || from_slot >= virtual_bag::kSlotCount) return op_err("bad slot");
    const bool from_original = virtual_bag::valid_original_transaction_bag(from_bag);
    const bool to_original = virtual_bag::valid_original_transaction_bag(to_bag);
    const int from_internal = extension_internal_bag_bridge(from_bag);
    const int to_internal = extension_internal_bag_bridge(to_bag);
    const bool from_extension = from_internal >= 0;
    const bool to_extension = to_internal >= 0;
    if (!from_original && !from_extension) return op_err("bad from bag");
    if (!to_original && !to_extension) return op_err("bad to bag");
    if (from_original && to_original) return op_err("use /api/item/inventory/move_item");
    ::qol::extension_bag::ReentryLock lock;
    ensure_state_loaded_locked();
    // 锁内复核当前 epoch 准入：readiness 与真正执行之间可能发生 detach/epoch 变化。
    if (!extension_bag_session_admitted_locked()) return extension_bag_not_ready_error_locked_bridge();
    bool moved = false;
    if (from_original && to_extension) {
        moved = move_original_to_extension_slot(lock, from_bag, from_slot, to_internal);
    } else if (from_extension && to_original) {
        moved = move_extension_to_original(lock, from_internal, from_slot, to_bag);
    } else {
        moved = move_extension_to_extension(from_internal, from_slot, to_internal, to_slot);
    }
    if (!moved) return op_err("move failed");
    return "{\"ok\":true,\"state\":" + extension_bag_status_json_locked_bridge() + "}";
}

}  // namespace qol::features::extension_bag::usecases
