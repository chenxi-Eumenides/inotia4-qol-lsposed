#include "plugins/extension_bag/http/query.h"

#include <cstdint>
#include <cstring>
#include <string>

std::string extension_bag_api_status_json_impl();

namespace qol::features::extension_bag {

namespace {

QolResult response_error(contracts::HttpRes* response, uint16_t status, const char* message) {
    if (response == nullptr) {
        return contracts::error(kQolErrInvalidArgument, message,
                                static_cast<uint32_t>(std::strlen(message)));
    }
    const std::string body = std::string("{\"ok\":false,\"error\":\"") + message + "\"}";
    if (response->body == nullptr || response->body_capacity < body.size()) {
        response->body_size = 0;
        response->status = 500;
        return contracts::error(kQolErrInternal, "http response buffer too small", 31);
    }
    std::memcpy(response->body, body.data(), body.size());
    response->body_size = static_cast<uint32_t>(body.size());
    response->status = status;
    const int32_t code = status == 400 ? kQolErrInvalidArgument : kQolErrInternal;
    return contracts::error(code, message, static_cast<uint32_t>(std::strlen(message)));
}

}  // namespace

QolResult status_query_handler(const contracts::HttpReq* request, contracts::HttpRes* response,
                               void* ctx) {
    (void)ctx;
    if (request == nullptr || request->method != contracts::HttpMethod::kGet) {
        return response_error(response, 400, "bad request");
    }
    if (response == nullptr || response->body == nullptr) {
        return contracts::error(kQolErrInvalidArgument, "bad response buffer", 19);
    }

    const std::string body = ::extension_bag_api_status_json_impl();
    if (response->body_capacity < body.size()) {
        return response_error(response, 500, "http response buffer too small");
    }
    std::memcpy(response->body, body.data(), body.size());
    response->body_size = static_cast<uint32_t>(body.size());
    response->status = 200;
    return contracts::ok();
}

}  // namespace qol::features::extension_bag
