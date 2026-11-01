#pragma once

#include <cstdint>
#include <string>

#include "contracts/game_ports/item_port.h"
#include "json_lite.h"

// item_facts HTTP owner 的响应构造：把 native 原始库存 JSON 富化为迁移前 Kotlin 输出，
// 静态名称来自 port 的启动期名称表。只在 HTTP handler 调用（不读游戏内存）。
namespace qol::features::core_item {

// {"bags":[...]}：每个袋、每个物品按迁移前 restructureItem 字段顺序输出。
bool enrich_inventory(const char* json, uint32_t size, const contracts::QolItemPort* port,
                      std::string* out);

// 以公开读面相同的 restructureItem 字段顺序富化单个原始物品对象。
bool enrich_item_value(const JsonValue& item, const contracts::QolItemPort* port,
                       std::string* out);

}  // namespace qol::features::core_item
