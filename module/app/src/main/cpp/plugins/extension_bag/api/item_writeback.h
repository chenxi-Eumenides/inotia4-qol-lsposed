#pragma once

#include "contracts/inventory_item_writeback.h"

namespace qol::features::extension_bag {

// provider 侧实现：把被游戏侧原地改写的物化对象同步回扩展袋 descriptor。
// 句柄不属于扩展袋时返回成功（无操作），供消费者无条件调用。
extern const qol::contracts::InventoryItemWritebackApi kInventoryItemWritebackApi;

}  // namespace qol::features::extension_bag
