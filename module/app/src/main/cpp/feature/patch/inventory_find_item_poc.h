#pragma once

#include <cstdint>

using InventoryFindItemCallback = void* (*)(int32_t category);

// Stage 4 single-function PoC: preserve the original result first, then use the
// logical inventory only when the original lookup misses.
void* inventory_find_item_original_first(int32_t category,
                                         InventoryFindItemCallback backup,
                                         InventoryFindItemCallback extension_fallback,
                                         bool& recursive_guard);
