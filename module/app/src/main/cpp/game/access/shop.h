#pragma once

#include <string>

// 游戏层：商店域语义化访问（T2l-sho 迁自 api/native/game_shop.cpp）。
// 货架槽数组（G_DEALSYSTEM_SALE_LIST_VMA，16B/槽：+0 flags bit0=空/已售、+8 item 指针）与
// 价格/位域/叠加数量读取原语收口在本层；上层只调用语义函数。
// 线程纪律：读游戏全局与物品对象，只在游戏主线程调用。

namespace qol::game {

// 货架商品列表 JSON：{"items":[{slot,category,count,price}...]}；未就绪/空架返回 {"items":[]}。
std::string shop_items_json();

}  // namespace qol::game
