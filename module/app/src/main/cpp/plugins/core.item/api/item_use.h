#pragma once

#include "contracts/item_use.h"

// core.item 内部的 item.use 能力实现：
// - kItemUseApi 由 plugin.cpp 注册为 item.use capability；
// - item_use_dispatch 只由 CHAR_UseItemEx wrapper 在调用原函数后调用；
// - item_use_reset 在 provider 停用/停用时清空订阅，避免悬空回调。
namespace qol::features::core_item {

extern const contracts::ItemUseApi kItemUseApi;

void item_use_dispatch(const contracts::QolItemUseEvent& event);
void item_use_reset();

}  // namespace qol::features::core_item
