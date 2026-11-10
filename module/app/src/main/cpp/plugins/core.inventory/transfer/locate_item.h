#pragma once

#include "contracts/bag.h"
#include "contracts/item_handle.h"

namespace qol::features::core_inventory::transfer::replace_item {

// 按 handle 定位当前槽：注册源先用 identify（32 位 QolBagRef），原生先用
// INVEN_FindItemSlot 直查，只有这两条直接路径都找不到时才回退原生空间扫描。
// 该入口仅供 core.inventory 内部 replace 分派使用，不作为跨插件契约发布。
QolResult locate_item(contracts::QolItemHandle item, contracts::QolBagRef* out_ref,
                      int32_t* out_slot);

}  // namespace qol::features::core_inventory::transfer::replace_item
