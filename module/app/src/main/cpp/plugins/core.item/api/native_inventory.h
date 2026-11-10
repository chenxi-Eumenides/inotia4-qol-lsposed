#pragma once

#include "contracts/inventory_access.h"
#include "contracts/host_sdk.h"

namespace qol::features::core_item {

void bind_native_inventory_sdk(const contracts::QolHostSdk* sdk);
void reset_native_inventory();
// 能力撤销时只清缓存接口指针，保留 host sdk 供下一次查询重新 require。
void invalidate_native_inventory_api();
const contracts::QolInventoryAccessApi* inventory_native_api();

}  // namespace qol::features::core_item
