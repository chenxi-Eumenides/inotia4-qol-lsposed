#pragma once

#include "contracts/http.h"

namespace qol::features::extension_bag {

QolResult status_query_handler(const contracts::HttpReq* request,
                               contracts::HttpRes* response, void* ctx);

}  // namespace qol::features::extension_bag
