#include "plugins/extension_bag/runtime/extension_bag_port.h"

#include "contracts/item_facts.h"
#include "contracts/sell_price.h"
#include "plugins/extension_bag/extension_bag_bridge.h"
#include "plugins/extension_bag/game_ui_virtbag.h"
#include "plugins/extension_bag/runtime/item_access.h"

bool extension_bag_enabled() { return virtual_bag_enabled(); }

int64_t extension_bag_sell_price(void* item, int count, bool apply_variant_discount) {
    const auto* facts = qol::features::extension_bag::extension_bag_item_facts();
    if (facts == nullptr || facts->sell_price == nullptr || item == nullptr || count <= 0) {
        return -1;
    }
    qol::contracts::QolItemHandle handle{};
    if (!extension_bag_item_handle_for_pointer(item, &handle)) return -1;
    int64_t unit_price = 0;
    if (!qol::contracts::succeeded(facts->sell_price(handle, &unit_price))) return -1;
    int64_t final_price = 0;
    if (!qol::contracts::sell_price::calculate(
            unit_price, static_cast<uint32_t>(count), apply_variant_discount ? 7u : 10u, 10u,
            &final_price)) {
        return -1;
    }
    return final_price;
}
