#pragma once

#include <cstdint>
#include <map>
#include <string>

namespace qol::features::core_item::write_result {

struct SlotKey {
    int32_t space_id;
    int32_t bag;
    int32_t slot;

    bool operator<(const SlotKey& other) const;
};

using InventorySnapshot = std::map<SlotKey, std::string>;

bool capture_inventory_snapshot(InventorySnapshot* out);
void append_changed_slots(std::string& body, const InventorySnapshot& before,
                          const InventorySnapshot& after);

}  // namespace qol::features::core_item::write_result
