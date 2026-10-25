#include "change_set.h"

namespace qol::features::core_item::model {

bool same_slot(const SlotChange& left, const SlotChange& right) {
    return left.space_id == right.space_id && left.bag == right.bag && left.slot == right.slot;
}

bool same_item(const contracts::QolInventoryItemRef& left,
               const contracts::QolInventoryItemRef& right) {
    return left.kind == right.kind && left.bag == right.bag && left.slot == right.slot &&
           left.category == right.category && left.count == right.count &&
           left.item.slot == right.item.slot && left.item.generation == right.item.generation &&
           left.space_id == right.space_id;
}

bool same_change(const SlotChange& left, const SlotChange& right) {
    return same_slot(left, right) && same_item(left.before, right.before) &&
           same_item(left.after, right.after);
}

bool append(ChangeSet* set, const SlotChange& change) {
    if (set == nullptr) return false;
    for (SlotChange& existing : set->changes) {
        if (!same_slot(existing, change)) continue;
        existing.after = change.after;
        return true;
    }
    set->changes.push_back(change);
    return true;
}

void dedupe(ChangeSet* set) {
    if (set == nullptr) return;
    for (std::size_t index = 0; index < set->changes.size(); ++index) {
        std::size_t next = index + 1;
        while (next < set->changes.size()) {
            if (!same_slot(set->changes[index], set->changes[next])) {
                ++next;
                continue;
            }
            set->changes[index].after = set->changes[next].after;
            for (std::size_t move = next + 1; move < set->changes.size(); ++move) {
                set->changes[move - 1] = set->changes[move];
            }
            set->changes.pop_back();
        }
    }
}

bool equal(const ChangeSet& left, const ChangeSet& right) {
    if (left.changes.size() != right.changes.size()) return false;
    for (std::size_t index = 0; index < left.changes.size(); ++index) {
        if (!same_change(left.changes[index], right.changes[index])) return false;
    }
    return true;
}

}  // namespace qol::features::core_item::model
