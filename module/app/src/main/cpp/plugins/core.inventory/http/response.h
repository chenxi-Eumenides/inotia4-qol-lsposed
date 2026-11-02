#pragma once

#include <cstdint>
#include <string>

#include "contracts/game_ports/item_port.h"
#include "json_lite.h"
#include "plugins/core.inventory/bags/snapshot_store.h"

// inventory HTTP owner 的响应构造：把 native 原始库存 JSON 富化为迁移前 Kotlin 输出，
// 静态名称来自 item port 的启动期名称表。只在 HTTP handler 调用（不读游戏内存）。
// 与 core.item 的同名实现保持暂存重复：本份只保留快照物品富化所需入口。
namespace qol::features::core_inventory {

// 以公开读面相同的 restructureItem 字段顺序富化单个原始物品对象。
bool enrich_item_value(const JsonValue& item, const contracts::QolItemPort* port,
                       std::string* out);

// 空间化快照项目，复用既有原始字段投影与名称富化顺序。
bool enrich_snapshot_item(int32_t space, int32_t bag, const bags::SlotRecord& item,
                          const contracts::QolItemPort* port, bool include_bag,
                          std::string* out);

}  // namespace qol::features::core_inventory
