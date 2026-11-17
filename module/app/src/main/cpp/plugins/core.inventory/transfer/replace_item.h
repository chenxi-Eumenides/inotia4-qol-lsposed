#pragma once

#include "contracts/bag.h"
#include "contracts/inventory_transfer.h"

namespace qol::features::core_inventory::transfer::replace_item {

// 提交已原地更新的物品内容。expected 必须仍是当前物品；内部按 handle 定位来源：
// 注册源走 identify，原生走 INVEN_FindItemSlot 直查，失败才回退扫描。native 原地
// 写回对象；provider 分派 QolBagSource::replace_item 更新 descriptor。成功返回同一 handle。
// 定位失败但对象仍存在（典型：装备槽里的物品不属于任何袋子）时，原位 payload 写入
// 成功且没有注册源认领该 handle，即按替换成功返回；out->ref 为空（无袋子），
// out->item 仍是 expected。
QolResult replace_item(contracts::QolItemHandle expected,
                       const contracts::QolBagTransfer* replacement,
                       contracts::QolBagMutationResult* out);

}  // namespace qol::features::core_inventory::transfer::replace_item
