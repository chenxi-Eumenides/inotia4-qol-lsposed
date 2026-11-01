#pragma once

#include <cstdint>

#include "contracts/bag.h"

namespace qol::features::core_inventory::transfer::native_transport {

using BagTransfer = contracts::QolBagTransfer;

QolResult export_item(int32_t space, contracts::QolItemHandle item, int32_t count,
                      BagTransfer* out);
// detached 句柄（create/load_item 产物）的序列化：与原生导出同格式，供注册源导入。
QolResult export_detached_item(contracts::QolItemHandle item, int32_t count, BagTransfer* out);
QolResult import_item(const BagTransfer& transfer, int32_t target_space,
                      int32_t preferred_bag, int32_t preferred_slot,
                      contracts::QolBagMutationResult* out);
QolResult remove_item(int32_t space, contracts::QolItemHandle expected, int32_t count,
                      contracts::QolBagMutationResult* out);
QolResult locate(int32_t space, contracts::QolItemHandle item,
                 contracts::QolBagRef* out, int32_t* out_slot);

}  // namespace qol::features::core_inventory::transfer::native_transport
