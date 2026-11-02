#pragma once

#include "contracts/inventory_projection.h"
#include "contracts/result.h"

namespace qol::features::extension_bag {

QolResult register_projection_callbacks();
void unregister_projection_callbacks();

// 本插件注册到 core.item 投影注册表的回调表（静态存储，永不为空）；
// merge wrapper 等内部消费者直接读该表，回调内部自带 projection_available 门禁。
const qol::contracts::InventoryProjectionCallbacks& projection_callbacks();

}  // namespace qol::features::extension_bag
