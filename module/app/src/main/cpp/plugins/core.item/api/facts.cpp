#include "plugins/core.item/runtime/core_item_internal.h"

#include <cstring>

namespace qol::features::core_item {

QolResult category(contracts::QolItemHandle item, int32_t* out_category) {
    if (g_port == nullptr || g_port->read.category == nullptr) return not_ready();
    return g_port->read.category(item, out_category);
}

QolResult item_type(contracts::QolItemHandle item, contracts::QolItemType* out_type) {
    if (g_port == nullptr || g_port->read.item_type == nullptr) return not_ready();
    return g_port->read.item_type(item, out_type);
}

QolResult is_no_sell(int32_t category_value, int32_t* out_value) {
    if (g_port == nullptr || g_port->read.is_no_sell == nullptr) return not_ready();
    return g_port->read.is_no_sell(category_value, out_value);
}

QolResult sell_price(contracts::QolItemHandle item, int64_t* out_price) {
    if (g_port == nullptr || g_port->read.sell_price == nullptr) return not_ready();
    return g_port->read.sell_price(item, out_price);
}

QolResult quote(contracts::QolItemHandle item, uint32_t sale_path,
                contracts::QolSaleQuote* out) {
    if (g_port == nullptr || g_port->read.quote == nullptr) return not_ready();
    return g_port->read.quote(item, sale_path, out);
}

QolResult cumulate_count(contracts::QolItemHandle item, int32_t* out_count) {
    if (g_port == nullptr || g_port->read.cumulate_count == nullptr) return not_ready();
    return g_port->read.cumulate_count(item, out_count);
}

QolResult item_ref_at(int32_t bag, int32_t slot, contracts::QolInventoryItemRef* out) {
    if (g_port == nullptr || g_port->read.item_ref_at == nullptr) return not_ready();
    return g_port->read.item_ref_at(bag, slot, out);
}

QolResult physical_item_at(int32_t bag, int32_t slot, contracts::QolItemHandle* out_item) {
    if (g_port == nullptr || g_port->read.physical_item_at == nullptr) {
        return not_ready();
    }
    return g_port->read.physical_item_at(bag, slot, out_item);
}

QolResult item_view(contracts::QolItemHandle item, contracts::QolItemInventoryView* out_view) {
    if (g_port == nullptr || g_port->read.inventory_item_view == nullptr) return not_ready();
    return g_port->read.inventory_item_view(item, out_view);
}

QolResult effective_count(contracts::QolItemHandle item, int32_t* out_count) {
    if (g_port == nullptr || g_port->read.effective_cumulate_count == nullptr) return not_ready();
    return g_port->read.effective_cumulate_count(item, out_count);
}

QolResult count_limit(contracts::QolItemHandle item, uint32_t* out_limit) {
    if (g_port == nullptr || g_port->read.count_limit == nullptr) return not_ready();
    return g_port->read.count_limit(item, out_limit);
}

QolResult count_encoding(contracts::QolItemHandle item,
                         contracts::QolItemCountEncoding* out_encoding) {
    if (g_port == nullptr || g_port->read.count_encoding == nullptr) return not_ready();
    return g_port->read.count_encoding(item, out_encoding);
}

QolResult detail_item_context(contracts::QolItemDetailContext* out_context) {
    if (g_port == nullptr || g_port->read.detail_item_context == nullptr) return not_ready();
    return g_port->read.detail_item_context(out_context);
}

QolResult money(int64_t* out_money) {
    if (g_port == nullptr || g_port->read.money == nullptr) return not_ready();
    return g_port->read.money(out_money);
}

extern const contracts::ItemFactsApi kItemFactsApi{
    sizeof(contracts::ItemFactsApi),
    &category,
    &item_type,
    &is_no_sell,
    &sell_price,
    &cumulate_count,
    &item_ref_at,
    &physical_item_at,
    &detail_item_context,
    &money,
    &item_view,
    &effective_count,
    &count_limit,
    &count_encoding,
    &quote,
};

const contracts::ItemFactsApi* item_facts_api() {
    return &kItemFactsApi;
}

}  // namespace qol::features::core_item
