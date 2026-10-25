#include "plugins/core.item/runtime/core_item_internal.h"

#include <algorithm>
#include <memory>
#include <utility>
#include <vector>

#include "plugins/core.item/bags/snapshot_store.h"

namespace qol::features::core_item {
namespace {

struct SpaceItems {
    int32_t space_id;
    std::vector<contracts::QolInventoryItemRef>* items;
};

int32_t collect_space_item(const contracts::QolInventoryItemRef* item, void* raw_context) {
    if (item == nullptr || raw_context == nullptr) return 0;
    auto* context = static_cast<SpaceItems*>(raw_context);
    if (item->space_id == context->space_id && item->bag >= 0 && item->slot >= 0 &&
        item->item.slot != 0 && item->item.generation != 0) {
        context->items->push_back(*item);
    }
    return 0;
}

bool item_is_not_found(const QolResult& result) {
    return result.code == kQolErrNotFound;
}

bool item_is_unavailable(const QolResult& result) {
    return result.code == kQolErrUnsupported || result.code == kQolErrNotReady;
}

QolResult read_space_count(int32_t space_id, int32_t* out_count) {
    if (out_count == nullptr) return error_result(kQolErrInvalidArgument, "invalid bag count");
    if (space_id == contracts::kQolBagSpaceOriginal) {
        *out_count = contracts::kQolOriginalBagCount;
        return contracts::ok();
    }
    *out_count = 0;
    if (g_port == nullptr || g_port->read.bag_count == nullptr ||
        !contracts::qol_item_port_bag_space_size_valid(g_port)) {
        return contracts::ok();
    }
    int32_t count = 0;
    const QolResult result = g_port->read.bag_count(space_id, &count);
    if (!contracts::succeeded(result)) {
        if (item_is_unavailable(result)) return contracts::ok();
        return result;
    }
    *out_count = count > 0 ? count : 0;
    return contracts::ok();
}

QolResult append_item_record(const contracts::QolInventoryItemRef& item, int32_t space_id,
                             int32_t bag, int32_t capacity, bags::BagSnapshot* snapshot) {
    if (snapshot == nullptr || item.space_id != space_id || item.bag != bag || item.slot < 0 ||
        item.slot >= capacity || item.item.slot == 0 || item.item.generation == 0) {
        return contracts::ok();
    }

    contracts::QolItemInventoryView view{};
    const QolResult view_result = g_port->read.inventory_item_view(item.item, &view);
    if (!contracts::succeeded(view_result)) {
        return item_is_not_found(view_result) ? contracts::ok() : view_result;
    }
    if (!inventory_option_counts_valid(view)) {
        return error_result(kQolErrInvalidArgument, "invalid item option count");
    }

    uint32_t count = 0;
    bool count_present = false;
    if (!effective_item_count(item.item, view.count_supported != 0, &count, &count_present)) {
        return error_result(kQolErrNotReady, "item count unavailable");
    }
    if (count_present) view.count_supported = 1;
    snapshot->items.push_back(
        bags::SlotRecord{item.slot, view, count_present ? count : 0});
    return contracts::ok();
}

QolResult read_bag(int32_t space_id, int32_t bag,
                   const std::vector<contracts::QolInventoryItemRef>* space_items,
                   bool has_space_items, bags::BagSnapshot* out_bag) {
    if (out_bag == nullptr || g_port == nullptr || g_port->read.bag_info == nullptr) {
        return error_result(kQolErrNotReady, "bag info unavailable");
    }
    const contracts::QolBagRef ref{space_id, bag};
    contracts::QolBagInfo info{};
    const QolResult info_result = g_port->read.bag_info(&ref, &info);
    if (!contracts::succeeded(info_result)) return info_result;
    if (info.capacity < 0 || info.slot_count < 0) {
        return error_result(kQolErrInvalidArgument, "invalid bag info");
    }

    bags::BagSnapshot snapshot{space_id, bag, info.capacity, info.slot_count, {}};
    if (has_space_items) {
        for (const contracts::QolInventoryItemRef& item : *space_items) {
            if (item.bag != bag) continue;
            const QolResult result = append_item_record(item, space_id, bag, info.capacity,
                                                        &snapshot);
            if (!contracts::succeeded(result)) return result;
        }
    } else {
        if (g_port->read.item_at == nullptr) {
            return error_result(kQolErrNotReady, "bag item read unavailable");
        }
        for (int32_t slot = 0; slot < info.capacity; ++slot) {
            contracts::QolInventoryItemRef item{};
            const QolResult item_result = g_port->read.item_at(&ref, slot, &item);
            if (!contracts::succeeded(item_result)) {
                if (item_is_not_found(item_result)) continue;
                return item_result;
            }
            const QolResult result = append_item_record(item, space_id, bag, info.capacity,
                                                        &snapshot);
            if (!contracts::succeeded(result)) return result;
        }
    }
    std::sort(snapshot.items.begin(), snapshot.items.end(),
              [](const bags::SlotRecord& lhs, const bags::SlotRecord& rhs) {
                  return lhs.slot < rhs.slot;
              });
    *out_bag = std::move(snapshot);
    return contracts::ok();
}

QolResult append_space(int32_t space_id, int32_t count, bags::InventorySnapshot* snapshot) {
    std::vector<contracts::QolInventoryItemRef> space_items;
    bool has_space_items = false;
    if (g_port != nullptr && g_port->read.for_each_space_slot != nullptr &&
        contracts::qol_item_port_bag_space_size_valid(g_port)) {
        SpaceItems context{space_id, &space_items};
        const QolResult result = g_port->read.for_each_space_slot(
            space_id, &collect_space_item, &context);
        if (contracts::succeeded(result)) {
            has_space_items = true;
        } else if (!item_is_unavailable(result)) {
            return result;
        }
    }

    for (int32_t bag = 0; bag < count; ++bag) {
        bags::BagSnapshot bag_snapshot{};
        const QolResult result = read_bag(space_id, bag, &space_items, has_space_items,
                                          &bag_snapshot);
        if (!contracts::succeeded(result)) return result;
        snapshot->bags.push_back(std::move(bag_snapshot));
    }
    return contracts::ok();
}

}  // namespace

QolResult read_inventory() {
    if (g_port == nullptr || g_port->read.in_world == nullptr ||
        g_port->read.inventory_item_view == nullptr || g_port->read.money == nullptr) {
        return not_ready();
    }
    if (g_port->read.static_ready != nullptr) {
        int32_t ready = 0;
        const QolResult ready_result = g_port->read.static_ready(&ready);
        if (!contracts::succeeded(ready_result)) return ready_result;
        if (ready == 0) return not_ready();
    }
    int32_t in_world = 0;
    const QolResult world_result = g_port->read.in_world(&in_world);
    if (!contracts::succeeded(world_result)) return world_result;
    if (in_world == 0) {
        bags::inventory_snapshot_store().clear();
        return not_ready();
    }

    auto next = std::make_shared<bags::InventorySnapshot>();
    for (const int32_t space_id : {contracts::kQolBagSpaceOriginal,
                                   contracts::kQolBagSpaceC18,
                                   contracts::kQolBagSpaceFirstRegistered}) {
        int32_t count = 0;
        const QolResult count_result = read_space_count(space_id, &count);
        if (!contracts::succeeded(count_result)) return count_result;
        const QolResult append_result = append_space(space_id, count, next.get());
        if (!contracts::succeeded(append_result)) return append_result;
    }
    const QolResult money_result = g_port->read.money(&next->money);
    if (!contracts::succeeded(money_result)) return money_result;

    bags::SnapshotStore& store = bags::inventory_snapshot_store();
    next->epoch = store.bump_epoch();
    store.publish(std::move(next));
    return contracts::ok();
}

void request_inventory_refresh() {
    bags::inventory_snapshot_store().bump_epoch();
    (void)read_inventory();
}

}  // namespace qol::features::core_item
