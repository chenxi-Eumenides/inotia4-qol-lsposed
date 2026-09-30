#pragma once

#include <cstdint>
#include <string>

#include "contracts/item_handle.h"

using LogicalInventoryItemFn = bool (*)(int bag, int slot, int category, int count,
                                        qol::contracts::QolItemHandle item, void* ctx);

bool extension_bag_enabled();
int64_t extension_bag_sell_price(void* item, int count, bool apply_variant_discount);
bool extension_bag_has_empty_slots(int needed, int include_task_bag);
void extension_bag_begin_internal_equip();
void extension_bag_end_internal_equip();
bool extension_bag_register_item_handle(void* item);
void extension_bag_invalidate_item_handle(void* item);
bool extension_bag_rebind_item_handle(void* item);
bool extension_bag_private_item_for_handle(qol::contracts::QolItemHandle handle, void** out_item);
bool extension_bag_item_handle_for_pointer(void* item, qol::contracts::QolItemHandle* out_handle);

namespace qol::features::extension_bag {

constexpr uint32_t kNativeBagObjectMarkerShift = 25;
constexpr uint32_t kNativeBagObjectMarkerMask = 0x7Fu << kNativeBagObjectMarkerShift;

// Native bag objects are non-encoded categories. The marker mask currently
// overlaps the S2 B segment, so callers must write only this marker and must
// not combine it with an item-count facade conversion.
inline uint32_t write_native_bag_object_marker(uint32_t value) {
    return (value & ~kNativeBagObjectMarkerMask) |
           (1u << kNativeBagObjectMarkerShift);
}

}  // namespace qol::features::extension_bag
