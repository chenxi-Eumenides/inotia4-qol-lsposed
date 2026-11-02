#include "plugins/extension_bag/usecases/move.h"
#include "plugins/extension_bag/runtime/reentry_lock.h"

#include "contracts/inventory_action.h"
#include "contracts/item_facts.h"
#include "plugins/extension_bag/api/bag_source.h"
#include "plugins/extension_bag/core/projection.h"
#include "plugins/extension_bag/model/virtual_bag_state.h"
#include "plugins/extension_bag/runtime/entry_guard.h"
#include "plugins/extension_bag/runtime/inventory_action.h"
#include "plugins/extension_bag/runtime/item_access.h"
#include "plugins/extension_bag/runtime/logging.h"
#include "plugins/extension_bag/runtime/session.h"
#include "plugins/extension_bag/runtime/shared.h"
#include "plugins/extension_bag/runtime/state.h"
#include "plugins/extension_bag/runtime/object_state_port.h"
#include "plugins/extension_bag/runtime/ownership_port.h"
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

bool move_extension_to_extension_via_action_locked(::qol::extension_bag::ReentryLock& lock,
                                                   int src_bag, int src_slot, int dst_bag,
                                                   int requested_dst_slot) {
    if (!lock.owns_lock() || !virtual_bag::valid_index(src_bag) ||
        !virtual_bag::valid_index(dst_bag) || src_slot < 0 ||
        src_slot >= virtual_bag::kSlotCount) {
        return false;
    }
    // 目标非空时保留旧事务的合并/交换语义；空槽移动统一交给 core.item。
    if (requested_dst_slot >= 0 &&
        !module_slot_is_assignable_locked(dst_bag, requested_dst_slot)) {
        const bool moved = ::move_extension_to_extension_locked(src_bag, src_slot, dst_bag,
                                                                requested_dst_slot);
        if (moved) (void)persist_state_locked();
        return moved;
    }

    void* item = module_item_locked(src_bag, src_slot);
    const virtual_bag::Item& descriptor =
        ::qol::extension_bag::access::bag().items[src_bag][src_slot];
    if (item == nullptr || descriptor.category <= 0 || descriptor.count <= 0) return false;
    const int count = static_cast<int>(
        virtual_bag_effective_count(item, static_cast<uint32_t>(descriptor.count)));
    if (count <= 0) return false;

    const auto* action = ::qol::features::extension_bag::inventory_action_api();
    if (action == nullptr) return false;

    int dst_slot = requested_dst_slot;
    const int capacity = ::qol::extension_bag::access::bag().capacities[dst_bag];
    if (dst_slot < 0) {
        // 自动落位先并入目标袋已有同身份堆，再新建空槽。判据与 source_import_item
        // 占用槽分支共用 first_mergeable_slot（排除 UID/数量位）；count 与导入
        // gate 一致（本次将移入的 effective count + 已有 descriptor count）。
        // 同袋时跳过源槽：同槽身份恒等，不排除会自并。
        const int merge_slot = virtual_bag::first_mergeable_slot(
            ::qol::extension_bag::access::bag().items, dst_bag, capacity, descriptor,
            virtual_bag_category_uses_stack_count, virtual_bag_count_limit(true),
            src_bag == dst_bag ? src_slot : -1);
        if (merge_slot >= 0) dst_slot = merge_slot;
    }
    if (dst_slot < 0) {
        for (int slot = 0; slot < capacity; ++slot) {
            if (module_slot_is_assignable_locked(dst_bag, slot)) {
                dst_slot = slot;
                break;
            }
        }
    }
    if (dst_slot < 0 || dst_slot >= capacity) return false;

    lock.unlock();
    const auto outcome = action->move(
        {qol::features::extension_bag::kExtensionBagSpaceId, src_bag, src_slot}, count,
        {qol::features::extension_bag::kExtensionBagSpaceId, dst_bag, dst_slot});
    lock.lock();

    if (outcome.status != qol::contracts::InventoryActionStatus::kOk ||
        outcome.moved_count <= 0) {
        QOL_LOG_WARN(QolDomain::kExtensionBag,
                     "extension->extension action rejected status=%d src=%d/%d dst=%d/%d",
                     static_cast<int>(outcome.status), src_bag, src_slot, dst_bag, dst_slot);
        return false;
    }
    refresh_projected_module_view_after_move_locked();
    QOL_LOG_INFO(QolDomain::kExtensionBag,
                 "extension->extension committed through inventory.action src=%d/%d dst=%d/%d "
                 "count=%d",
                 src_bag, src_slot, dst_bag, dst_slot, outcome.moved_count);
    return true;
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
        moved = move_extension_to_extension_via_action_locked(lock, from_internal, from_slot,
                                                              to_internal, to_slot);
    }
    return moved ? MoveResult{true, Error::kNone} : MoveResult{false, Error::kMoveFailed};
}

}  // namespace qol::features::extension_bag::usecases
