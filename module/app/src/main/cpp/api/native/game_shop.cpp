// game_shop.cpp —— 商店域：商店货架物品（parse 域）
// 由 game_misc.cpp 拆分生成（纯搬代码，零逻辑变更）。

#include "game_shop.h"

#include "game_access.h"
#include "game_state.h"
#include "game_ops_common.h"

std::string data_shop_items_json() {
    if (g_base == 0 || fn_item_get_buy_price == nullptr || fn_get_bit == nullptr || fn_get_cumulate_count == nullptr) return "{\"items\":[]}";
    uint8_t* sale_list = reinterpret_cast<uint8_t*>(*(reinterpret_cast<void**>(g_base + G_DEALSYSTEM_SALE_LIST_VMA)));
    if (sale_list == nullptr) return "{\"items\":[]}";
    std::string s = "{\"items\":[";
    bool first = true;
    for (int i = 0; i < 48; ++i) {
        uint8_t* slot = sale_list + i * 16;
        uint64_t flags = *reinterpret_cast<uint64_t*>(slot);
        if (flags & 1) continue;  // bit0=空/已售
        void* item = *reinterpret_cast<void**>(slot + 8);
        if (item == nullptr) continue;
        uint16_t iflags = *reinterpret_cast<uint16_t*>(reinterpret_cast<uint8_t*>(item) + I_TYPE);
        uint32_t category = fn_get_bit(iflags, 15, 6);
        uint32_t count = fn_get_cumulate_count(item);
        int price = fn_item_get_buy_price(item);
        if (!first) s += ",";
        first = false;
        s += "{\"slot\":" + std::to_string(i) + ",\"category\":" + std::to_string(category) +
             ",\"count\":" + std::to_string(count) + ",\"price\":" + std::to_string(price) + "}";
    }
    s += "]}";
    return s;
}

std::string data_op_shop_buy(int32_t slot) {
    if (!game_in_world()) return op_err("not in game");
    if (fn_item_get_buy_price == nullptr || fn_get_money == nullptr || fn_minus_money == nullptr ||
        fn_inven_find_save_slot == nullptr || fn_inven_save_item == nullptr)
        return op_err("symbol not resolved");
    if (slot < 0 || slot >= 48) return op_err("bad slot");
    uint8_t* sale_list = reinterpret_cast<uint8_t*>(*(reinterpret_cast<void**>(g_base + G_DEALSYSTEM_SALE_LIST_VMA)));
    if (sale_list == nullptr) return op_err("no shop");
    uint8_t* slot_ptr = sale_list + slot * 16;
    uint64_t flags = *reinterpret_cast<uint64_t*>(slot_ptr);
    if (flags & 1) return op_err("item sold out");
    void* item = *reinterpret_cast<void**>(slot_ptr + 8);
    if (item == nullptr) return op_err("item not found");
    int price = fn_item_get_buy_price(item);
    int64_t money = fn_get_money();
    if (money < price) return op_err("not enough money");
    int save_slot = fn_inven_find_save_slot(item, 0);
    if (save_slot <= 0) return op_err("inventory full");
    if (!fn_inven_save_item(item, nullptr)) return op_err("buy failed");
    fn_minus_money(price);
    return "{\"ok\":true,\"price\":" + std::to_string(price) + "}";
}
