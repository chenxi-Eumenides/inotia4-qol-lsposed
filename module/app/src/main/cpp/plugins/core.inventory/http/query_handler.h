#pragma once

#include <cstdint>

#include "contracts/http.h"
#include "contracts/result.h"

namespace qol::features::core_inventory {

// inventory HTTP 读侧 5 条 GET 的 query handler。数据只来自已发布快照；
// 快照不可用时按 not ready 回 503，不读 live 状态。
QolResult response_error(contracts::HttpRes* response, uint16_t status, const char* message);
QolResult response_body(contracts::HttpRes* response, const char* body, uint32_t size,
                        uint16_t status = 200);

enum class Query : uint8_t {
    kInventory,
    kMoney,
    kItems,
    kBagInfo,
    kBagSlot,
};

extern Query kQueries[];
QolResult query_handler(const contracts::HttpReq* request, contracts::HttpRes* response, void* ctx);

}  // namespace qol::features::core_inventory
