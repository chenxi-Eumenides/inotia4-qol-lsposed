#pragma once

#include <vector>

#include "contracts/game_ports/item_port.h"

namespace qol::features::core_item::model {

struct SlotChange {
    int32_t space_id;
    int32_t bag;
    int32_t slot;
    contracts::QolInventoryItemRef before;
    contracts::QolInventoryItemRef after;
};

struct ChangeSet {
    std::vector<SlotChange> changes;
};

bool same_slot(const SlotChange& left, const SlotChange& right);
bool same_item(const contracts::QolInventoryItemRef& left,
               const contracts::QolInventoryItemRef& right);
bool same_change(const SlotChange& left, const SlotChange& right);
bool append(ChangeSet* set, const SlotChange& change);
void dedupe(ChangeSet* set);
bool equal(const ChangeSet& left, const ChangeSet& right);

}  // namespace qol::features::core_item::model
