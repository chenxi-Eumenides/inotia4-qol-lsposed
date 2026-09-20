#pragma once

// HTTP 注册口契约：只描述固定宽度 POD 和调用方缓冲区，不承载传输实现。

#include <cstdint>

#include "result.h"
#include "version.h"

namespace qol::contracts {

enum class HttpMethod : uint8_t {
    kGet = 0,
    kPost = 1,
};

struct HttpRoute {
    HttpMethod method;
    const char* path;
    const char* owner;
    uint8_t op_gated;
    uint8_t lease_required;
};

struct HttpReq {
    HttpMethod method;
    const char* path;
    uint32_t path_len;
    const uint8_t* body;
    uint32_t body_len;
};

struct HttpRes {
    uint8_t* body;
    uint32_t body_capacity;
    uint32_t body_size;
    uint16_t status;
};

using HttpRegisterRouteFn = QolResult (*)(const HttpRoute* route);
using HttpUnregisterOwnerFn = QolResult (*)(const char* owner);
using HttpFindRouteFn = QolResult (*)(HttpMethod method, const char* path,
                                      const HttpRoute** out_route);
using HttpListRoutesFn = QolResult (*)(HttpRoute* out_routes, uint32_t capacity,
                                       uint32_t* out_count);
using HttpLeaseEnterFn = QolResult (*)(HttpMethod method, const char* path,
                                       uint64_t* out_lease);
using HttpLeaseLeaveFn = QolResult (*)(uint64_t lease);

struct QolHttpRegistryApi {
    HttpRegisterRouteFn register_route;
    HttpUnregisterOwnerFn unregister_owner;
    HttpFindRouteFn find;
    HttpListRoutesFn list;
    HttpLeaseEnterFn lease_enter;
    HttpLeaseLeaveFn lease_leave;
};

constexpr const char* kHttpRegistryCapabilityId = "http.registry";
constexpr QolVersion kHttpRegistryVersion{1, 0};

}  // namespace qol::contracts
