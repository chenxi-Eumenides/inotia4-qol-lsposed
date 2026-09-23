#pragma once

#include <cstdint>
#include <string>

#include "contracts/game_ports/item_port.h"

// item_facts HTTP owner 的响应构造：把 native 原始库存/商店 JSON 富化为迁移前 Kotlin 输出，
// 静态名称来自 port 的启动期名称表。只在 HTTP handler 调用（不读游戏内存）。
namespace qol::features::core_item {

// {"bags":[...]}：每个袋、每个物品按迁移前 restructureItem 字段顺序输出。
bool enrich_inventory(const char* json, uint32_t size, const contracts::QolItemPort* port,
                      std::string* out);

// {"items":[...]}：展平全部袋内物品（物品对象与库存内同构，不含 bag 字段）。
bool enrich_items_list(const char* json, uint32_t size, const contracts::QolItemPort* port,
                       std::string* out);

// {"bag":b,"capacity":c,"slot_count":n}；bag 不存在时 *found=false。
bool enrich_bag_info(const char* json, uint32_t size, int32_t bag, std::string* out, bool* found);

// 指定袋内指定槽的富化物品；*bag_found=false 表示袋不存在（调用方回 404），
// 袋存在但槽为空时写入 "null"。
bool enrich_bag_slot(const char* json, uint32_t size, int32_t bag, int32_t slot,
                     const contracts::QolItemPort* port, std::string* out, bool* bag_found);

// {"items":[...]}：商店货架物品追加 name（category 无名称时不加）。
bool enrich_shop_items(const char* json, uint32_t size, const contracts::QolItemPort* port,
                       std::string* out);

}  // namespace qol::features::core_item
