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

struct HttpPathParam {
    const char* name;
    uint32_t name_len;
    const char* value;
    uint32_t value_len;
};

struct HttpReq {
    // query/path_params are tail fields so existing aggregate initialization remains valid.
    HttpMethod method;
    const char* path;
    uint32_t path_len;
    const uint8_t* body;
    uint32_t body_len;
    const char* query;
    uint32_t query_len;
    const HttpPathParam* path_params;
    uint32_t path_param_count;
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

using HttpQueryHandlerFn = QolResult (*)(const HttpReq* request, HttpRes* response, void* ctx);
using HttpCommandHandlerFn = QolResult (*)(const HttpReq* request, HttpRes* response, void* ctx);
struct HttpQueryHandlerDescriptor {
    HttpMethod method;
    const char* path;
    const char* owner;
    HttpQueryHandlerFn handler;
    void* ctx;
};
using HttpRegisterQueryHandlerFn = QolResult (*)(HttpMethod method, const char* path,
                                                 const char* owner, HttpQueryHandlerFn handler,
                                                 void* ctx);
using HttpRegisterCommandHandlerFn = QolResult (*)(HttpMethod method, const char* path,
                                                   const char* owner,
                                                   HttpCommandHandlerFn handler, void* ctx);
using HttpUnregisterHandlerOwnerFn = QolResult (*)(const char* owner);
using HttpDispatchFn = QolResult (*)(const HttpReq* request, HttpRes* response);

struct QolHttpRegistryApi {
    HttpRegisterRouteFn register_route;
    HttpUnregisterOwnerFn unregister_owner;
    HttpFindRouteFn find;
    HttpListRoutesFn list;
    HttpLeaseEnterFn lease_enter;
    HttpLeaseLeaveFn lease_leave;
};

struct QolHttpDispatchApi {
    HttpRegisterQueryHandlerFn register_query;
    HttpRegisterCommandHandlerFn register_command;
    HttpUnregisterHandlerOwnerFn unregister_owner;
    HttpDispatchFn dispatch;
};

constexpr const char* kHttpRegistryCapabilityId = "http.registry";
constexpr QolVersion kHttpRegistryVersion{1, 1};
constexpr const char* kHttpDispatchCapabilityId = "http.dispatch";
constexpr QolVersion kHttpDispatchVersion{1, 1};

}  // namespace qol::contracts
