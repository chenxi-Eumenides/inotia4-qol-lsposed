#include "plugins/extension_bag/usecases/move.h"
#include "plugins/extension_bag/runtime/reentry_lock.h"

#include "contracts/item_facts.h"
#include "plugins/extension_bag/runtime/entry_guard.h"
#include "plugins/extension_bag/runtime/session.h"
#include "plugins/extension_bag/runtime/shared.h"
#include "plugins/extension_bag/runtime/state.h"
#include "plugins/extension_bag/runtime/object_state_port.h"
#include "plugins/extension_bag/transaction/transaction.h"

namespace qol::features::extension_bag::usecases {

bool move_original_to_extension_slot(
    ::qol::extension_bag::ReentryLock& lock, int src_bag, int src_slot, int dst_bag,
    qol::contracts::QolItemHandle expected_item) {
    return ::move_original_to_extension_slot_locked(lock, src_bag, src_slot, dst_bag,
                                                    expected_item);
}

bool move_original_to_extension(::qol::extension_bag::ReentryLock& lock, int src_bag,
                                int src_slot, int dst_bag,
                                qol::contracts::QolItemHandle expected_item) {
    return ::move_original_to_extension_slot_locked(lock, src_bag, src_slot, dst_bag,
                                                    expected_item);
}

bool move_extension_to_original(::qol::extension_bag::ReentryLock& lock, int src_bag,
                                int src_slot, int target_bag) {
    return ::move_extension_to_original_locked(lock, src_bag, src_slot, target_bag);
}

bool move_extension_to_extension(int src_bag, int src_slot, int dst_bag, int dst_slot) {
    return ::move_extension_to_extension_locked(src_bag, src_slot, dst_bag, dst_slot);
}

MoveResult move_item(int from_bag, int from_slot, int to_bag, int to_slot) {
    if (!qol::extension_bag::extension_bag_on_main_thread()) {
        return {false, runtime_state().enabled.load() ? Error::kNotInGame : Error::kDisabled};
    }
    if (!extension_bag_ready_locked()) {
        return {false, runtime_state().enabled.load() ? Error::kNotInGame : Error::kDisabled};
    }
    if (module_view_installed_port()) {
        return {false, Error::kMovementDisabled};
    }
    if (from_bag == virtual_bag::kOriginalTaskBag ||
        to_bag == virtual_bag::kOriginalTaskBag) {
        return {false, Error::kTaskBagExcluded};
    }
    if (from_slot < 0 || from_slot >= virtual_bag::kSlotCount) return {false, Error::kBadSlot};
    const bool from_original = virtual_bag::valid_original_transaction_bag(from_bag);
    const bool to_original = virtual_bag::valid_original_transaction_bag(to_bag);
    const int from_internal = extension_storage_index(from_bag);
    const int to_internal = extension_storage_index(to_bag);
    const bool from_extension = from_internal >= 0;
    const bool to_extension = to_internal >= 0;
    if (!from_original && !from_extension) return {false, Error::kBadSourceBag};
    if (!to_original && !to_extension) return {false, Error::kBadDestinationBag};
    if (from_original && to_original) return {false, Error::kUseInventoryMove};
    ::qol::extension_bag::ReentryLock lock;
    // 锁内复核当前 epoch 准入：readiness 与真正执行之间可能发生 detach/epoch 变化。
    if (!extension_bag_session_admitted_locked()) {
        return {false, runtime_state().enabled.load() ? Error::kNotInGame : Error::kDisabled};
    }
    bool moved = false;
    if (from_original && to_extension) {
        qol::contracts::QolItemHandle expected_item = qol::contracts::kInvalidItemHandle;
        const auto* facts = runtime_state().host.item_facts;
        qol::contracts::QolInventoryItemRef source{};
        if (facts == nullptr || facts->item_ref_at == nullptr ||
            !qol::contracts::succeeded(facts->item_ref_at(from_bag, from_slot, &source)) ||
            source.bag != from_bag || source.slot != from_slot || source.item.slot == 0 ||
            source.item.generation == 0) {
            return {false, Error::kMoveFailed};
        }
        expected_item = source.item;
        moved = move_original_to_extension_slot(lock, from_bag, from_slot, to_internal,
                                                expected_item);
    } else if (from_extension && to_original) {
        moved = move_extension_to_original(lock, from_internal, from_slot, to_bag);
    } else {
        moved = move_extension_to_extension(from_internal, from_slot, to_internal, to_slot);
    }
    return moved ? MoveResult{true, Error::kNone} : MoveResult{false, Error::kMoveFailed};
}

}  // namespace qol::features::extension_bag::usecases
