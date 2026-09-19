#pragma once

#include "contracts/game_host.h"

// 游戏层：库存只读遍历与引用适配（ARCHITECTURE.md §5）。
// 物理袋/扩展袋的底层布局和扩展背包端口均留在 game/ 与既有宿主实现内；插件只接收
// contracts 中的不透明库存引用。

namespace qol::game {

// 按袋号和槽号读取库存引用；空槽返回 kQolErrNotFound，底层尚未就绪返回
// kQolErrNotReady。
QolResult inventory_item_ref_at(int32_t bag, int32_t slot,
                               contracts::QolInventoryItemRef* out);

// 遍历原版物理袋 0..5；回调返回 1 时停止。回调期间引用有效，返回后不得保存。
QolResult for_each_bag_slot(contracts::QolInventoryBagSlotFn callback, void* ctx);

}  // namespace qol::game
