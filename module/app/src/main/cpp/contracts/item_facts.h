#pragma once

#include <cstdint>

#include "game_ports/item_port.h"
#include "result.h"
#include "version.h"

namespace qol::contracts {

constexpr const char* kItemFactsCapabilityId = "item.facts";
constexpr QolVersion kItemFactsVersion{3, 0};
constexpr uint16_t kItemFactsVersionMajor = kItemFactsVersion.major;
constexpr uint16_t kItemFactsVersionMinor = kItemFactsVersion.minor;

using ItemFactsCategoryFn = QolResult (*)(QolItemHandle item, int32_t* out_category);
using ItemFactsIsRealEquipFn = QolResult (*)(QolItemHandle item, int32_t* out_value);
using ItemFactsIsJewelCategoryFn = QolResult (*)(int32_t category, int32_t* out_is_jewel);
using ItemFactsIsNoSellFn = QolResult (*)(int32_t category, int32_t* out_value);
using ItemFactsSellPriceFn = QolResult (*)(QolItemHandle item, int64_t* out_price);
using ItemFactsCumulateCountFn = QolResult (*)(QolItemHandle item, int32_t* out_count);
using ItemFactsViewFn = QolResult (*)(QolItemHandle item, QolItemInventoryView* out_view);
using ItemFactsEffectiveCountFn = QolResult (*)(QolItemHandle item, int32_t* out_count);

using ItemFactsItemRefAtFn = QolResult (*)(int32_t bag, int32_t slot,
                                           QolInventoryItemRef* out);
using ItemFactsPhysicalItemAtFn = QolResult (*)(int32_t bag, int32_t slot,
                                                QolItemHandle* out_item);
using ItemFactsDetailItemContextFn = QolResult (*)(QolItemDetailContext* out_context);
using ItemFactsMoneyFn = QolResult (*)(int64_t* out_money);

struct ItemFactsApi {
    uint32_t struct_size;
    ItemFactsCategoryFn category;
    ItemFactsIsRealEquipFn is_real_equip;
    ItemFactsIsJewelCategoryFn is_jewel_category;
    ItemFactsIsNoSellFn is_no_sell;
    ItemFactsSellPriceFn sell_price;
    ItemFactsCumulateCountFn cumulate_count;
    ItemFactsItemRefAtFn item_ref_at;
    ItemFactsPhysicalItemAtFn physical_item_at;
    ItemFactsDetailItemContextFn detail_item_context;
    ItemFactsMoneyFn money;
    ItemFactsViewFn view;
    ItemFactsEffectiveCountFn effective_count;
};

inline bool item_facts_api_size_valid(const ItemFactsApi* api) {
    return api != nullptr && api->struct_size >= sizeof(ItemFactsApi);
}

}  // namespace qol::contracts
