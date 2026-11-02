#include "plugins/core.item/runtime/core_item_internal.h"

#include <cstring>
#include <string>

namespace qol::features::core_item {

// 库存读侧 5 条 GET 的 query handler 已迁往 core.inventory（plugins/core.inventory/http/）；
// 本文件只剩 HTTP 响应构造 helper，供 shop query 与写命令 handler 共用。

QolResult response_error(contracts::HttpRes* response, uint16_t status, const char* message) {
    const std::string body = std::string("{\"ok\":false,\"error\":\"") + message + "\"}";
    if (response == nullptr || response->body == nullptr || response->body_capacity < body.size()) {
        return contracts::error(kQolErrInternal, "http response buffer too small", 31);
    }
    std::memcpy(response->body, body.data(), body.size());
    response->body_size = static_cast<uint32_t>(body.size());
    response->status = status;
    return contracts::error(status == 503 ? kQolErrNotReady : kQolErrInvalidArgument, message,
                            static_cast<uint32_t>(std::strlen(message)));
}

QolResult response_body(contracts::HttpRes* response, const char* body, uint32_t size,
                        uint16_t status) {
    if (response == nullptr || response->body == nullptr || body == nullptr ||
        response->body_capacity < size) {
        return response_error(response, 500, "http response buffer too small");
    }
    std::memcpy(response->body, body, size);
    response->body_size = size;
    response->status = status;
    return contracts::ok();
}

}  // namespace qol::features::core_item
