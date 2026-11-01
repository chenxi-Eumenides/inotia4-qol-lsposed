#pragma once

#include "contracts/inventory_transfer.h"

namespace qol::features::core_inventory::transfer {

// inventory.transfer 能力实现：move_item 编排跨空间转移，serialize_item 走
// detached 句柄序列化，locate 按 space 分派 native / 注册源。
const contracts::QolInventoryTransferApi& inventory_transfer_api();

}  // namespace qol::features::core_inventory::transfer
