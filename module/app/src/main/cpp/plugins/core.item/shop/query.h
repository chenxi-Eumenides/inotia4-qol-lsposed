#pragma once

#include "contracts/http.h"

namespace qol::features::core_item::shop {

QolResult query_handler(const contracts::HttpReq* request, contracts::HttpRes* response,
                        void* ctx);

}  // namespace qol::features::core_item::shop
