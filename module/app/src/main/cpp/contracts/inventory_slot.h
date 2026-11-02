#pragma once

#include <cstdint>

#include "item_handle.h"
#include "result.h"
#include "version.h"

namespace qol::contracts {

constexpr const char* kInventorySlotCapabilityId = "inventory.slot";
constexpr QolVersion kInventorySlotVersion{1, 0};

struct QolInventorySlotView {
    uint32_t struct_size;
    QolItemHandle item;
    uint32_t kind;
    int32_t bag;
    int32_t slot;
    int32_t category;
    int32_t count;
    uint32_t encoding;
    uint32_t flags;
};

enum QolInventoryRemoveReason : uint32_t {
    kRemoveSell = 0,
    kRemoveDiscard = 1,
};

struct QolInventorySlotApi {
    uint32_t struct_size;
    QolResult (*resolve)(int32_t bag, int32_t slot, QolInventorySlotView* out);
    QolResult (*view)(QolItemHandle item, QolInventorySlotView* out);
    QolResult (*remove)(int32_t bag, int32_t slot, QolItemHandle expected, uint32_t reason);
};

}  // namespace qol::contracts
