#include "inventory_find_item_poc.h"

void* inventory_find_item_original_first(int32_t category,
                                         InventoryFindItemCallback backup,
                                         InventoryFindItemCallback extension_fallback,
                                         bool& recursive_guard) {
    if (recursive_guard || backup == nullptr) {
        return backup == nullptr ? nullptr : backup(category);
    }
    recursive_guard = true;
    void* result = backup(category);
    if (result == nullptr && extension_fallback != nullptr) {
        result = extension_fallback(category);
    }
    recursive_guard = false;
    return result;
}
