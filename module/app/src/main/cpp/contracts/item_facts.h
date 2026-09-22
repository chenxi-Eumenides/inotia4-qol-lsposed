#pragma once

#include <cstdint>

#include "game_ports/item_port.h"
#include "result.h"
#include "version.h"

namespace qol::contracts {

constexpr const char* kItemFactsCapabilityId = "item.facts";
constexpr QolVersion kItemFactsVersion{1, 0};
constexpr uint16_t kItemFactsVersionMajor = kItemFactsVersion.major;
constexpr uint16_t kItemFactsVersionMinor = kItemFactsVersion.minor;

using ItemFactsCategoryFn = QolResult (*)(const void* item, int32_t* out_category);
using ItemFactsIsRealEquipFn = QolResult (*)(const void* item, int32_t* out_value);
using ItemFactsIsJewelCategoryFn = QolResult (*)(int32_t category, int32_t* out_is_jewel);
using ItemFactsIsNoSellFn = QolResult (*)(int32_t category, int32_t* out_value);
using ItemFactsSellPriceFn = QolResult (*)(const void* item, int64_t* out_price);
using ItemFactsCumulateCountFn = QolResult (*)(const void* item, int32_t* out_count);

using ItemFactsItemRefAtFn = QolResult (*)(int32_t bag, int32_t slot,
                                           QolInventoryItemRef* out);
using ItemFactsPhysicalItemAtFn = QolResult (*)(int32_t bag, int32_t slot, void** out_item);
using ItemFactsDetailContextFn = QolResult (*)(int32_t* out_desc_type, int32_t* out_bag,
                                               void** out_control);
using ItemFactsCursorIndexFn = QolResult (*)(const void* control, int32_t* out_slot);

struct ItemFactsApi {
    ItemFactsCategoryFn category;
    ItemFactsIsRealEquipFn is_real_equip;
    ItemFactsIsJewelCategoryFn is_jewel_category;
    ItemFactsIsNoSellFn is_no_sell;
    ItemFactsSellPriceFn sell_price;
    ItemFactsCumulateCountFn cumulate_count;
    ItemFactsItemRefAtFn item_ref_at;
    ItemFactsPhysicalItemAtFn physical_item_at;
    ItemFactsDetailContextFn detail_context;
    ItemFactsCursorIndexFn cursor_index;
};

}  // namespace qol::contracts
