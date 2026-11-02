#include "inventory_store.h"

#include <cstring>
#include <limits>
#include <utility>

namespace qol::features::core_inventory::store {
namespace {

QolResult fail(int32_t code, const char* message) {
    return contracts::error(code, message, static_cast<uint32_t>(std::strlen(message)));
}

bool valid_ref(const contracts::QolInventorySlotRef& ref) {
    return ref.space_id >= 0 && ref.bag >= 0 && ref.slot >= 0;
}

bool is_task_bag(const contracts::QolInventorySlotRef& ref) {
    return ref.space_id == contracts::kQolBagSpaceOriginal &&
           ref.bag == contracts::kQolTaskBagIndex;
}

bool valid_item(const contracts::QolInventoryItem& item) {
    return item.item.slot != 0 && item.item.generation != 0 && item.category >= 0 &&
           item.count > 0;
}

void fill_ref(int32_t space_id, int32_t bag, int32_t slot,
              const contracts::QolInventoryItem& item,
              contracts::QolInventoryItemRef* out) {
    out->kind = space_id >= contracts::kQolBagSpaceFirstRegistered
                    ? static_cast<int32_t>(contracts::QolInventoryItemKind::kProvider)
                    : static_cast<int32_t>(contracts::QolInventoryItemKind::kOriginal);
    out->bag = bag;
    out->slot = slot;
    out->category = item.category;
    out->count = item.count;
    out->item = item.item;
    out->space_id = space_id;
}

}  // namespace

bool InventoryStore::add_table(std::unique_ptr<SlotTable> table) {
    if (table == nullptr) return false;
    if (find_table(table->space_id()) != nullptr) return false;
    tables_.push_back(std::move(table));
    return true;
}

void InventoryStore::clear() { tables_.clear(); }

SlotTable* InventoryStore::find_table(int32_t space_id) const {
    for (const auto& table : tables_) {
        if (table != nullptr && table->space_id() == space_id) return table.get();
    }
    return nullptr;
}

QolResult InventoryStore::item_at(const contracts::QolInventorySlotRef& ref,
                                  contracts::QolInventoryItemRef* out) const {
    if (out == nullptr || !valid_ref(ref)) {
        return fail(kQolErrInvalidArgument, "invalid inventory reference");
    }
    SlotTable* table = find_table(ref.space_id);
    if (table == nullptr) return fail(kQolErrNotFound, "space not registered");

    contracts::QolInventoryItem item{};
    if (!table->get(ref.bag, ref.slot, &item)) {
        return fail(kQolErrNotFound, "slot empty");
    }
    fill_ref(ref.space_id, ref.bag, ref.slot, item, out);
    return contracts::ok();
}

QolResult InventoryStore::locate(contracts::QolItemHandle item,
                                 contracts::QolInventoryItemRef* out) const {
    if (out == nullptr || item.slot == 0 || item.generation == 0) {
        return fail(kQolErrInvalidArgument, "invalid item handle");
    }
    for (const auto& table : tables_) {
        if (table == nullptr) continue;
        for (int32_t bag = 0; bag < table->bag_count(); ++bag) {
            const int32_t capacity = table->capacity(bag);
            for (int32_t slot = 0; slot < capacity; ++slot) {
                contracts::QolInventoryItem candidate{};
                if (!table->get(bag, slot, &candidate)) continue;
                if (candidate.item.slot == item.slot &&
                    candidate.item.generation == item.generation) {
                    fill_ref(table->space_id(), bag, slot, candidate, out);
                    return contracts::ok();
                }
            }
        }
    }
    return fail(kQolErrNotFound, "item handle not located");
}

QolResult InventoryStore::find_first(int32_t category,
                                     contracts::QolInventoryItemRef* out) const {
    if (out == nullptr || category < 0) {
        return fail(kQolErrInvalidArgument, "invalid category");
    }
    for (const auto& table : tables_) {
        if (table == nullptr) continue;
        for (int32_t bag = 0; bag < table->bag_count(); ++bag) {
            const int32_t capacity = table->capacity(bag);
            for (int32_t slot = 0; slot < capacity; ++slot) {
                contracts::QolInventoryItem candidate{};
                if (!table->get(bag, slot, &candidate)) continue;
                if (candidate.category != category) continue;
                fill_ref(table->space_id(), bag, slot, candidate, out);
                return contracts::ok();
            }
        }
    }
    return fail(kQolErrNotFound, "category not found");
}

QolResult InventoryStore::find_empty(int32_t space_id,
                                     contracts::QolInventorySlotRef* out) const {
    if (out == nullptr || space_id < 0) {
        return fail(kQolErrInvalidArgument, "invalid space");
    }
    SlotTable* table = find_table(space_id);
    if (table == nullptr) return fail(kQolErrNotFound, "space not registered");

    for (int32_t bag = 0; bag < table->bag_count(); ++bag) {
        const int32_t capacity = table->capacity(bag);
        for (int32_t slot = 0; slot < capacity; ++slot) {
            contracts::QolInventoryItem ignored{};
            if (table->get(bag, slot, &ignored)) continue;
            out->space_id = space_id;
            out->bag = bag;
            out->slot = slot;
            return contracts::ok();
        }
    }
    return fail(kQolErrNotFound, "no empty slot");
}

QolResult InventoryStore::count_item(int32_t category, int32_t* out_count) const {
    if (out_count == nullptr || category < 0) {
        return fail(kQolErrInvalidArgument, "invalid category");
    }
    int64_t total = 0;
    for (const auto& table : tables_) {
        if (table == nullptr) continue;
        for (int32_t bag = 0; bag < table->bag_count(); ++bag) {
            const int32_t capacity = table->capacity(bag);
            for (int32_t slot = 0; slot < capacity; ++slot) {
                contracts::QolInventoryItem candidate{};
                if (!table->get(bag, slot, &candidate)) continue;
                if (candidate.category != category) continue;
                total += candidate.count;
                if (total > std::numeric_limits<int32_t>::max()) {
                    *out_count = std::numeric_limits<int32_t>::max();
                    return contracts::ok();
                }
            }
        }
    }
    *out_count = static_cast<int32_t>(total);
    return contracts::ok();
}

QolResult InventoryStore::move(const contracts::QolInventorySlotRef& from,
                               const contracts::QolInventorySlotRef& to) {
    if (!valid_ref(from) || !valid_ref(to)) {
        return fail(kQolErrInvalidArgument, "invalid inventory reference");
    }
    if (is_task_bag(from) || is_task_bag(to)) {
        return fail(kQolErrBlocked, "task bag is read-only");
    }
    if (from.space_id == to.space_id && from.bag == to.bag && from.slot == to.slot) {
        return contracts::ok();
    }

    SlotTable* from_table = find_table(from.space_id);
    SlotTable* to_table = find_table(to.space_id);
    if (from_table == nullptr || to_table == nullptr) {
        return fail(kQolErrNotFound, "space not registered");
    }

    contracts::QolInventoryItem item{};
    if (!from_table->get(from.bag, from.slot, &item)) {
        return fail(kQolErrNotFound, "source slot empty");
    }
    contracts::QolInventoryItem occupied{};
    if (to_table->get(to.bag, to.slot, &occupied)) {
        return fail(kQolErrConflict, "destination slot occupied");
    }
    if (!to_table->set(to.bag, to.slot, item)) {
        return fail(kQolErrInternal, "destination write failed");
    }
    if (!from_table->clear(from.bag, from.slot)) {
        (void)to_table->clear(to.bag, to.slot);
        return fail(kQolErrInternal, "source clear failed");
    }
    return contracts::ok();
}

QolResult InventoryStore::take(const contracts::QolInventorySlotRef& ref,
                               contracts::QolInventoryItem* out) {
    if (out == nullptr || !valid_ref(ref)) {
        return fail(kQolErrInvalidArgument, "invalid inventory reference");
    }
    if (is_task_bag(ref)) return fail(kQolErrBlocked, "task bag is read-only");

    SlotTable* table = find_table(ref.space_id);
    if (table == nullptr) return fail(kQolErrNotFound, "space not registered");

    contracts::QolInventoryItem item{};
    if (!table->get(ref.bag, ref.slot, &item)) {
        return fail(kQolErrNotFound, "slot empty");
    }
    if (!table->clear(ref.bag, ref.slot)) {
        return fail(kQolErrInternal, "source clear failed");
    }
    *out = item;
    return contracts::ok();
}

QolResult InventoryStore::place(const contracts::QolInventorySlotRef& ref,
                                const contracts::QolInventoryItem& item) {
    if (!valid_ref(ref) || !valid_item(item)) {
        return fail(kQolErrInvalidArgument, "invalid inventory item");
    }
    if (is_task_bag(ref)) return fail(kQolErrBlocked, "task bag is read-only");

    SlotTable* table = find_table(ref.space_id);
    if (table == nullptr) return fail(kQolErrNotFound, "space not registered");

    contracts::QolInventoryItem occupied{};
    if (table->get(ref.bag, ref.slot, &occupied)) {
        return fail(kQolErrConflict, "destination slot occupied");
    }
    if (!table->set(ref.bag, ref.slot, item)) {
        return fail(kQolErrInternal, "destination write failed");
    }
    return contracts::ok();
}

QolResult InventoryStore::remove_count(const contracts::QolInventorySlotRef& ref,
                                       int32_t count) {
    if (!valid_ref(ref) || count <= 0) {
        return fail(kQolErrInvalidArgument, "invalid remove count");
    }
    if (is_task_bag(ref)) return fail(kQolErrBlocked, "task bag is read-only");

    SlotTable* table = find_table(ref.space_id);
    if (table == nullptr) return fail(kQolErrNotFound, "space not registered");

    contracts::QolInventoryItem item{};
    if (!table->get(ref.bag, ref.slot, &item)) {
        return fail(kQolErrNotFound, "slot empty");
    }
    if (count > item.count) return fail(kQolErrConflict, "remove count exceeds stack");

    if (count == item.count) {
        if (!table->clear(ref.bag, ref.slot)) {
            return fail(kQolErrInternal, "source clear failed");
        }
        return contracts::ok();
    }

    item.count -= count;
    if (!table->set(ref.bag, ref.slot, item)) {
        return fail(kQolErrInternal, "source write failed");
    }
    return contracts::ok();
}

}  // namespace qol::features::core_inventory::store
