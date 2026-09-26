#pragma once

#include <string>

// 游戏层：legacy 背包 JSON 读面（T2l-inv 迁自 api/native/game_inventory.{h,cpp}+read.inc）。
// 供 cache 槽 6 legacy builder（host 注入）与 item_port 读面共用同一实现。
// 线程纪律：读游戏内存与物品对象，只在游戏主线程调用。

void append_item_attrs(std::string& s, void* item);
std::string build_inventory_json();
