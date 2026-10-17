#include "game/access/item_sell_quote.h"

#include <limits>

#include "contracts/item_count.h"
#include "contracts/sell_price.h"
#include "game/access/game_access.h"
#include "game/access/item_class.h"
#include "game/adapter_ports/item_count_facade.h"

namespace qol::game::item_sell_quote {

bool quote(void* item, uint32_t sale_path, GameSeries series, contracts::QolSaleQuote* out) {
    if (out == nullptr) return false;
    *out = {};
    if (item == nullptr) return false;

    const int unit_price = item_sell_price(item);
    if (unit_price < 0) return false;

    uint32_t count = 0;
    const contracts::QolItemCountApi* count_api = item_count_api_if_resident();
    if (count_api != nullptr && count_api->canonical != nullptr &&
        count_api->view_from_canonical != nullptr) {
        uint32_t canonical = 0;
        if (!contracts::succeeded(count_api->canonical(item, &canonical)) ||
            !contracts::succeeded(count_api->view_from_canonical(canonical, 1, &count))) {
            return false;
        }
    } else {
        if (fn_get_cumulate_count == nullptr) return false;
        const int legacy_count = fn_get_cumulate_count(item);
        if (legacy_count <= 0) return false;
        count = static_cast<uint32_t>(legacy_count);
    }
    if (count == 0 || count > static_cast<uint32_t>(std::numeric_limits<int32_t>::max())) {
        return false;
    }

    uint32_t multiplier_num = 0;
    uint32_t multiplier_den = 0;
    if (!sell_multiplier(series, sale_path, &multiplier_num, &multiplier_den)) return false;

    int64_t price = 0;
    if (!contracts::sell_price::calculate(unit_price, count, multiplier_num, multiplier_den,
                                          &price)) {
        return false;
    }

    out->struct_size = sizeof(*out);
    out->unit_price = unit_price;
    out->count = static_cast<int32_t>(count);
    out->price = price;
    out->multiplier_num = multiplier_num;
    out->multiplier_den = multiplier_den;
    return true;
}

}  // namespace qol::game::item_sell_quote
