#pragma once

#include <cstdint>

namespace qol::contracts {

// Shared main-thread signal for the CHAR_UnequipItemToInven call stack.
// The action hook lives in core.item; the save-slot hook lives in core.inventory.
// A contracts-level inline thread_local keeps the signal in one entity across the
// shared library without a plugin-to-plugin include or direct symbol dependency.
inline thread_local int32_t g_inventory_unequip_depth = 0;

struct InventoryUnequipScope {
    InventoryUnequipScope() { ++g_inventory_unequip_depth; }
    ~InventoryUnequipScope() {
        if (g_inventory_unequip_depth > 0) --g_inventory_unequip_depth;
    }
    InventoryUnequipScope(const InventoryUnequipScope&) = delete;
    InventoryUnequipScope& operator=(const InventoryUnequipScope&) = delete;
};

}  // namespace qol::contracts
