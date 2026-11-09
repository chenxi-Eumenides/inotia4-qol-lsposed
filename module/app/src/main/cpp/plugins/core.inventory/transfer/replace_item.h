#pragma once

#include "contracts/bag.h"
#include "contracts/inventory_transfer.h"

namespace qol::features::core_inventory::transfer::replace_item {

// 提交已原地更新的物品内容。expected 必须仍是当前物品；native 只确认，provider
// 分派 QolBagSource::replace_item 更新 descriptor。成功返回同一 handle。
QolResult replace_item(contracts::QolItemHandle expected,
                       const contracts::QolBagTransfer* replacement,
                       contracts::QolBagMutationResult* out);

}  // namespace qol::features::core_inventory::transfer::replace_item
