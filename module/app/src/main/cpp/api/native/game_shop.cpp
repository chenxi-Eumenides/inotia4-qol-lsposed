// game_shop.cpp —— 商店域：商店货架物品（parse 域）
// 由 game_misc.cpp 拆分生成（纯搬代码，零逻辑变更）。

#include "game_shop.h"

#include <mutex>

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

// 购买互斥：原版购买链「查钱→入库→扣钱→清货架」非原子（UI 单线程天然串行），
// API 的 HTTP 并发请求可重入同链导致同帧双扣/双入库，必须串行化。
static std::mutex g_shop_buy_mtx;

// 复刻原版 UIStore_BuyItem(0xd242c) 的普通商品路径，顺序与分支一一对应：
//   ① INVEN_GetMoney 与价格比较（0xd2440），不足 → 错误（原版弹窗 text 0xa）
//   ② INVEN_FindSaveSlot 预检（0xd24a0，buy-gate patch 点），无空位 → 错误（原版弹窗 text 0xb）
//   ③ INVEN_SaveItem 入包（0xd24b0）
//   ④ 入包成功后才 INVEN_MinusMoney 扣钱（0xd2560）——先入库后扣钱，入库失败不动钱
//   ⑤ DEALSYSTEM_ClearItem 把商品从货架摘除（0xd2568）——同一 item 指针不得同时留在
//      背包与货架，漏掉此步货架槽仍持有该指针且 bit0 未置位 → 同槽重复购买 = 物品复制
// 弹窗链检查：购买本身无确认弹窗（BuyItem 直接执行，0xa/0xb/0xc 均为结果提示窗且
// fpOK=null 无后续逻辑），API 用前置错误返回等价拦截提示窗分支，直连执行安全。
// 与原版的偏离（均有原因）：
//   - 原版从 UI 控件取选中商品（ControlObject_GetCursor→ControlItem_GetItem），API 无 UI
//     上下文，改为按调用方 slot 直读货架槽，入口语义等价。
//   - 原版对 DEALSYSTEM_FindStatic 未命中的商品走 ITEMSYSTEM_CopyAsNewUID 复制路径（0xd24ec，
//     稀有度商店动态商品），普通 NPC 商店货架商品均命中 FindStatic 主路径；API 仅服务普通
//     商店货架，不实现复制路径。
//   - 原版成功后调 UIStore_UpdateBuyItemGroupControl/RefreshInvenItem 刷新商店 UI（0xd256c），
//     API 无商店 UI，跳过；op_ok() 内 frame_cache_force_refresh 已刷新 API 数据缓存。
//   - 原版末尾对损坏商品（ITEM_IsRealBroken）补写耐久标记位（0xd2588），属展示语义，API 不复刻。
std::string data_op_shop_buy(int32_t slot) {
    if (!game_in_world()) return op_err("not in game");
    if (fn_item_get_buy_price == nullptr || fn_get_money == nullptr || fn_minus_money == nullptr ||
        fn_inven_find_save_slot == nullptr || fn_inven_save_item == nullptr ||
        fn_dealsystem_clear_item == nullptr)
        return op_err("symbol not resolved");
    if (slot < 0 || slot >= 48) return op_err("bad slot");
    std::lock_guard<std::mutex> lock(g_shop_buy_mtx);
    uint8_t* sale_list = reinterpret_cast<uint8_t*>(*(reinterpret_cast<void**>(g_base + G_DEALSYSTEM_SALE_LIST_VMA)));
    if (sale_list == nullptr) return op_err("no shop");
    uint8_t* slot_ptr = sale_list + slot * 16;
    uint64_t flags = *reinterpret_cast<uint64_t*>(slot_ptr);
    if (flags & 1) return op_err("item sold out");
    void* item = *reinterpret_cast<void**>(slot_ptr + 8);
    if (item == nullptr) return op_err("item not found");
    int price = fn_item_get_buy_price(item);
    // ① 原版先查钱（0xd2440 INVEN_GetMoney 比较），不足直接失败，不动任何状态
    int64_t money = fn_get_money();
    if (money < price) return op_err("not enough money");
    // ② 原版入库前先找空槽（0xd24a0），无空位失败，不动钱
    int8_t save_slot = -1;
    if (!fn_inven_find_save_slot(item, &save_slot)) return op_err("inventory full");
    // ③④ 对齐原版顺序：先 SaveItem 入包，成功后才扣钱（0xd24b0 → 0xd2560）
    if (!fn_inven_save_item(item, nullptr)) return op_err("buy failed");
    if (!fn_minus_money(price)) {
        // 理论不可达（①已确认余额充足且本函数持锁串行）；防御性回滚：钱已扣但入库成功，
        // 无法安全回退入库（物品可能已并入堆叠），保留入库、报告扣钱失败
        return op_err("money update failed");
    }
    // ⑤ 原版 ClearItem 摘除货架商品（0xd2568），防止同槽重复购买复制物品
    fn_dealsystem_clear_item(item);
    return "{\"ok\":true,\"price\":" + std::to_string(price) + "}";
}
