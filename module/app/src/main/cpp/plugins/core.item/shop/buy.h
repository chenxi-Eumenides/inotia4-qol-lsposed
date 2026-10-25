#pragma once

#include "plugins/core.item/runtime/core_item_internal.h"

namespace qol::features::core_item::shop {

QolResult buy_command(const contracts::HttpReq* request, contracts::HttpRes* response,
                      void* ctx);
bool write_result_if_requested(const contracts::QolItemWriteArgs& args, char* out,
                               uint32_t capacity, uint32_t* out_size, bool* handled);

extern const ItemWriteRoute kBuyRoute;

}  // namespace qol::features::core_item::shop
