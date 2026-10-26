#include "plugins/extension_bag/api/bag_source.h"
#include "plugins/extension_bag/core/projection.h"
#include "plugins/extension_bag/model/virtual_bag_state.h"
#include "plugins/extension_bag/runtime/inventory_access.h"
#include "plugins/extension_bag/runtime/inventory_action.h"
#include "plugins/extension_bag/runtime/item_access.h"
#include "plugins/extension_bag/runtime/logging.h"
#include "plugins/extension_bag/runtime/ownership_port.h"
#include "plugins/extension_bag/runtime/projection_controller.h"
#include "plugins/extension_bag/runtime/reentry_lock.h"
#include "plugins/extension_bag/runtime/state.h"

#include "contracts/game_host.h"
#include "contracts/game_ports/item_port.h"
#include "contracts/inventory_action.h"
#include "contracts/item_facts.h"

namespace {

int32_t native_space_for_bag(int bag) {
    return bag >= qol::contracts::kQolOriginalBagCount
               ? qol::contracts::kQolBagSpaceVariant
               : qol::contracts::kQolBagSpaceOriginal;
}

int32_t native_bag_number(int bag) {
    return bag >= qol::contracts::kQolOriginalBagCount
               ? bag - qol::contracts::kQolOriginalBagCount
               : bag;
}

bool first_empty_extension_slot_locked(int bag, int* out_slot) {
    if (out_slot == nullptr || !virtual_bag::valid_index(bag)) return false;
    const int capacity = ::qol::extension_bag::access::bag().capacities[bag];
    for (int slot = 0; slot < capacity; ++slot) {
        if (module_slot_is_assignable_locked(bag, slot)) {
            *out_slot = slot;
            return true;
        }
    }
    return false;
}

bool native_slot_empty_locked(int bag, int slot) {
    const auto* facts = ::qol::extension_bag::access::host().item_facts;
    if (facts == nullptr || facts->physical_item_at == nullptr) return false;
    qol::contracts::QolItemHandle handle{};
    const QolResult result = facts->physical_item_at(bag, slot, &handle);
    if (result.code == kQolErrNotFound) return true;
    if (!qol::contracts::succeeded(result)) return false;
    return handle.slot == 0 || handle.generation == 0;
}

bool first_empty_native_slot_locked(int bag, int* out_slot) {
    if (out_slot == nullptr) return false;
    int32_t size = 0;
    if (!native_bag_size_locked(bag, &size) || size <= 0) {
        return false;
    }
    for (int slot = 0; slot < size && slot < virtual_bag::kSlotCount; ++slot) {
        if (native_slot_empty_locked(bag, slot)) {
            *out_slot = slot;
            return true;
        }
    }
    return false;
}

}  // namespace

bool move_original_to_extension_slot_locked(::qol::extension_bag::ReentryLock& lock, int src_bag,
                                            int src_slot, int dst_bag,
                                            qol::contracts::QolItemHandle expected_item) {
    if (!lock.owns_lock() || !native_source_bag_allowed_locked(src_bag) || src_slot < 0 ||
        src_slot >= virtual_bag::kSlotCount || !virtual_bag::valid_index(dst_bag)) {
        QOL_LOG_WARN(QolDomain::kExtensionBag,
                     "cross move reject original->extension source=%d/%d dst=%d", src_bag, src_slot,
                     dst_bag);
        return false;
    }

    qol::contracts::QolInventoryItemRef source{};
    if (::qol::extension_bag::access::host().item_facts == nullptr ||
        ::qol::extension_bag::access::host().item_facts->item_ref_at == nullptr ||
        !qol::contracts::succeeded(
            ::qol::extension_bag::access::host().item_facts->item_ref_at(src_bag, src_slot,
                                                                         &source)) ||
        source.bag != src_bag || source.slot != src_slot || source.item.slot == 0 ||
        source.item.generation == 0 || source.count <= 0 ||
        (expected_item.slot != 0 &&
         (source.item.slot != expected_item.slot ||
          source.item.generation != expected_item.generation))) {
        QOL_LOG_WARN(QolDomain::kExtensionBag,
                     "cross move reject original->extension stale source=%d/%d", src_bag, src_slot);
        return false;
    }

    int dst_slot = -1;
    if (!first_empty_extension_slot_locked(dst_bag, &dst_slot)) {
        QOL_LOG_WARN(QolDomain::kExtensionBag,
                     "cross move reject original->extension destination full dst=%d", dst_bag);
        return false;
    }

    const auto* action = ::qol::features::extension_bag::inventory_action_api();
    if (action == nullptr) {
        QOL_LOG_ERROR(QolDomain::kExtensionBag,
                      "cross move reject original->extension inventory.action unavailable");
        return false;
    }

    // 非堆叠原生装备的 cumulate_count 可能返回 0；移动这种物品时按单件处理。
    const int32_t move_count = source.count > 0 ? source.count : 1;
    lock.unlock();
    const auto outcome = action->move(
        {native_space_for_bag(src_bag), native_bag_number(src_bag), src_slot}, move_count,
        {qol::features::extension_bag::kExtensionBagSpaceId, dst_bag, dst_slot});
    lock.lock();

    if (outcome.status != qol::contracts::InventoryActionStatus::kOk ||
        outcome.moved_count <= 0) {
        QOL_LOG_WARN(QolDomain::kExtensionBag,
                     "cross move rejected original->extension status=%d source=%d/%d dst=%d/%d "
                     "count=%d",
                     static_cast<int>(outcome.status), src_bag, src_slot, dst_bag, dst_slot,
                     move_count);
        return false;
    }
    refresh_projected_module_view_after_move_locked();
    QOL_LOG_INFO(QolDomain::kExtensionBag,
                 "cross move committed through inventory.action original->extension source=%d/%d "
                 "dst=%d/%d count=%d",
                 src_bag, src_slot, dst_bag, dst_slot, outcome.moved_count);
    return true;
}

bool move_extension_to_original_locked(::qol::extension_bag::ReentryLock& lock, int src_bag,
                                       int src_slot, int target_bag) {
    const bool native_target = target_bag >= qol::contracts::kQolOriginalBagCount;
    if (!lock.owns_lock() || !virtual_bag::valid_index(src_bag) || src_slot < 0 ||
        src_slot >= virtual_bag::kSlotCount ||
        (!native_target && !virtual_bag::valid_original_transaction_bag(target_bag))) {
        QOL_LOG_WARN(QolDomain::kExtensionBag,
                     "cross move reject extension->original source=%d/%d target=%d", src_bag,
                     src_slot, target_bag);
        return false;
    }
    if (native_target) {
        const auto* game = ::qol::extension_bag::access::host().game;
        int32_t target_size = 0;
        const bool candidate =
            game != nullptr && game->target.has_capability != nullptr &&
            game->target.has_capability(
                qol::contracts::kQolTargetCapabilityC18NativeBagLayoutCandidate) != 0;
        const bool sized = native_bag_size_locked(target_bag, &target_size) &&
                           target_size > 0;
        if (!candidate || !sized) {
            QOL_LOG_WARN(QolDomain::kExtensionBag,
                         "cross move reject extension->native target=%d candidate=%d size=%d",
                         target_bag, candidate ? 1 : 0, target_size);
            return false;
        }
    }

    void* item = module_item_locked(src_bag, src_slot);
    const virtual_bag::Item& descriptor =
        ::qol::extension_bag::access::bag().items[src_bag][src_slot];
    if (item == nullptr || descriptor.category <= 0 || descriptor.count <= 0) {
        QOL_LOG_WARN(QolDomain::kExtensionBag,
                     "cross move reject extension->original source empty=%d/%d", src_bag, src_slot);
        return false;
    }
    const int count = static_cast<int>(
        virtual_bag_effective_count(item, static_cast<uint32_t>(descriptor.count)));
    if (count <= 0) return false;

    int target_slot = -1;
    if (!first_empty_native_slot_locked(target_bag, &target_slot)) {
        QOL_LOG_WARN(QolDomain::kExtensionBag,
                     "cross move reject extension->original target full=%d", target_bag);
        return false;
    }

    const auto* action = ::qol::features::extension_bag::inventory_action_api();
    if (action == nullptr) {
        QOL_LOG_ERROR(QolDomain::kExtensionBag,
                      "cross move reject extension->original inventory.action unavailable");
        return false;
    }

    lock.unlock();
    const auto outcome = action->move(
        {qol::features::extension_bag::kExtensionBagSpaceId, src_bag, src_slot}, count,
        {native_space_for_bag(target_bag), native_bag_number(target_bag), target_slot});
    lock.lock();

    if (outcome.status != qol::contracts::InventoryActionStatus::kOk ||
        outcome.moved_count <= 0) {
        QOL_LOG_WARN(QolDomain::kExtensionBag,
                     "cross move rejected extension->original status=%d source=%d/%d target=%d/%d",
                     static_cast<int>(outcome.status), src_bag, src_slot, target_bag, target_slot);
        return false;
    }
    refresh_projected_module_view_after_move_locked();
    QOL_LOG_INFO(QolDomain::kExtensionBag,
                 "cross move committed through inventory.action extension->original source=%d/%d "
                 "target=%d/%d count=%d",
                 src_bag, src_slot, target_bag, target_slot, outcome.moved_count);
    return true;
}
