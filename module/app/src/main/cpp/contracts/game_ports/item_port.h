#pragma once

#include <cstdint>

#include "../result.h"
#include "../version.h"

namespace qol::contracts {

enum class QolInventoryItemKind : int32_t {
    kOriginal = 0,
    kExtension = 1,
};

struct QolInventoryItemRef {
    int32_t kind;
    int32_t bag;
    int32_t slot;
    int32_t category;
    int32_t count;
    void* native_item;
};

using QolInventoryBagSlotFn = int32_t (*)(const QolInventoryItemRef* item, void* ctx);

struct QolItemPortReadApi {
    QolResult (*category)(const void* item, int32_t* out_category);
    QolResult (*is_real_equip)(const void* item, int32_t* out_value);
    QolResult (*is_jewel_category)(int32_t category, int32_t* out_is_jewel);
    QolResult (*is_no_sell)(int32_t category, int32_t* out_value);
    QolResult (*sell_price)(const void* item, int64_t* out_price);
    QolResult (*cumulate_count)(const void* item, int32_t* out_count);
    QolResult (*item_ref_at)(int32_t bag, int32_t slot, QolInventoryItemRef* out);
    QolResult (*physical_item_at)(int32_t bag, int32_t slot, void** out_item);
    QolResult (*detail_context)(int32_t* out_desc_type, int32_t* out_bag, void** out_control);
    QolResult (*cursor_index)(const void* control, int32_t* out_slot);
};

struct QolItemPort {
    QolVersion abi_version;
    QolItemPortReadApi read;
};

}  // namespace qol::contracts
