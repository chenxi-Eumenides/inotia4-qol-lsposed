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
#include "plugins/extension_bag/transaction/transaction.h"

#include "contracts/game_host.h"
#include "contracts/game_ports/item_port.h"
#include "contracts/inventory_action.h"
#include "contracts/item_facts.h"

#include <array>
#include <cstdint>

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
    int occupied = 0;
    for (int slot = 0; slot < capacity; ++slot) {
        const virtual_bag::Item& descriptor =
            ::qol::extension_bag::access::bag().items[bag][slot];
        if (descriptor.category > 0 || descriptor.count > 0 || descriptor.payload_size > 0) {
            ++occupied;
        }
        if (module_slot_is_assignable_locked(bag, slot)) {
            *out_slot = slot;
            return true;
        }
    }
    // "destination full" must be distinguishable from a slot poisoned by stale
    // object/handle/use state: only the latter has free capacity left.
    if (occupied < capacity) {
        QOL_LOG_WARN(QolDomain::kExtensionBag,
                     "cross move reject original->extension no assignable slot bag=%d capacity=%d occupied=%d",
                     bag, capacity, occupied);
    }
    return false;
}

// 原始→扩展自动落位的合并槽：目标袋内第一个与 source 同身份可堆叠（排除 UID/
// 数量位）且并入后不超过扩展堆上限的占用槽。判据与 QolBagSource 占用槽导入
// 分支共用 first_mergeable_slot 单源。
bool first_mergeable_extension_slot_locked(int bag, const virtual_bag::Item& source,
                                           int* out_slot) {
    if (out_slot == nullptr || !virtual_bag::valid_index(bag)) return false;
    const int slot = virtual_bag::first_mergeable_slot(
        ::qol::extension_bag::access::bag().items, bag,
        ::qol::extension_bag::access::bag().capacities[bag], source,
        virtual_bag_category_uses_stack_count, virtual_bag_count_limit(true),
        /*skip_slot=*/-1);
    if (slot < 0) return false;
    *out_slot = slot;
    return true;
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
        source.item.generation == 0 ||
        (expected_item.slot != 0 &&
         (source.item.slot != expected_item.slot ||
          source.item.generation != expected_item.generation))) {
        QOL_LOG_WARN(QolDomain::kExtensionBag,
                     "cross move reject original->extension stale source=%d/%d", src_bag, src_slot);
        return false;
    }

    // 原始→扩展自动落位先取源 payload 身份：合并判据需要与目标扩展堆逐位比较
    // （排除 UID/数量位）。序列化失败保持整体失败，不静默改挑空槽。
    void* source_item = nullptr;
    std::array<uint8_t, virtual_bag::kSerializedItemBuffer> source_payload{};
    int source_payload_size = 0;
    if (!inventory_slot_locked(src_bag, src_slot, &source_item) || source_item == nullptr ||
        !serialize_item_payload_locked(source_item, &source_payload, &source_payload_size)) {
        QOL_LOG_WARN(QolDomain::kExtensionBag,
                     "cross move reject original->extension source serialize failed=%d/%d",
                     src_bag, src_slot);
        return false;
    }
    virtual_bag::Item source_desc{};
    source_desc.category = source.category;
    // 非堆叠原生装备的 cumulate_count 可能返回 0；移动这种物品时按单件处理。
    source_desc.count = source.count > 0 ? source.count : 1;
    source_desc.payload = source_payload;
    source_desc.payload_size = source_payload_size;

    int dst_slot = -1;
    if (!first_mergeable_extension_slot_locked(dst_bag, source_desc, &dst_slot) &&
        !first_empty_extension_slot_locked(dst_bag, &dst_slot)) {
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

    const int32_t move_count = source_desc.count;
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
        invalidate_original_inventory_draw_cache_locked();
        return false;
    }
    invalidate_original_inventory_draw_cache_locked();
    refresh_native_item_area_after_move_locked(src_bag);
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
    int32_t target_size = 0;
    if (native_target) {
        const auto* game = ::qol::extension_bag::access::host().game;
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
    } else if (!native_bag_size_locked(target_bag, &target_size) || target_size <= 0) {
        QOL_LOG_WARN(QolDomain::kExtensionBag,
                     "cross move reject extension->original target size unavailable=%d",
                     target_bag);
        return false;
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

    const auto* action = ::qol::features::extension_bag::inventory_action_api();
    if (action == nullptr) {
        QOL_LOG_ERROR(QolDomain::kExtensionBag,
                      "cross move reject extension->original inventory.action unavailable");
        return false;
    }

    lock.unlock();
    const auto outcome = action->move(
        {qol::features::extension_bag::kExtensionBagSpaceId, src_bag, src_slot}, count,
        {native_space_for_bag(target_bag), native_bag_number(target_bag), -1});
    lock.lock();

    const bool target_valid =
        outcome.target.space_id == native_space_for_bag(target_bag) &&
        outcome.target.bag == native_bag_number(target_bag) && outcome.target.slot >= 0 &&
        outcome.target.slot < target_size && outcome.target.slot < virtual_bag::kSlotCount &&
        outcome.target_item.slot != 0 && outcome.target_item.generation != 0;
    if (outcome.status != qol::contracts::InventoryActionStatus::kOk ||
        outcome.moved_count <= 0 || !target_valid) {
        QOL_LOG_WARN(QolDomain::kExtensionBag,
                     "cross move rejected extension->original status=%d source=%d/%d "
                     "target=%d ref=%d/%d/%d handle=%u/%u",
                     static_cast<int>(outcome.status), src_bag, src_slot, target_bag,
                     outcome.target.space_id, outcome.target.bag, outcome.target.slot,
                     outcome.target_item.slot, outcome.target_item.generation);
        invalidate_original_inventory_draw_cache_locked();
        return false;
    }
    invalidate_original_inventory_draw_cache_locked();
    refresh_native_item_area_after_move_locked(target_bag);
    QOL_LOG_INFO(QolDomain::kExtensionBag,
                 "cross move committed through inventory.action extension->original source=%d/%d "
                 "target=%d/%d count=%d",
                 src_bag, src_slot, target_bag, outcome.target.slot, outcome.moved_count);
    return true;
}
