#pragma once

#include <cstdint>
#include <vector>

#include "contracts/http.h"

namespace qol::host {
using ResidentHttpRoute = bool (*)(const contracts::HttpReq&);
using ResidentHttpDispatch = QolResult (*)(const contracts::HttpReq&, contracts::HttpRes&);
void http_dispatch_bind_resident_control(ResidentHttpRoute, ResidentHttpDispatch);

// Host SDK 注入插件的 HTTP handler 接缝。
contracts::QolHttpDispatchApi http_dispatch_api();
// Frame lifecycle uses begin/poll; compatibility unregister never waits for in-flight handlers.
void http_dispatch_begin_unregister_owner(const char* owner);
bool http_dispatch_poll_unregister_owner(const char* owner);
bool http_dispatch_poll_all_inflight();
QolResult http_dispatch_dynamic(const contracts::HttpReq* request,
                                std::vector<uint8_t>* response_body, uint32_t max_capacity,
                                uint16_t* out_status, uint32_t* out_body_size);

// 插件描述符尾部的 query handler 声明由宿主生命周期调用。
QolResult http_dispatch_register_queries(const contracts::HttpQueryHandlerDescriptor* handlers,
                                         uint32_t count);

// 试点 JNI 使用的内置 handler 注册入口；业务插件仍通过 Host SDK 注册。
QolResult http_dispatch_register_builtin_query(contracts::HttpMethod method, const char* path,
                                               const char* owner,
                                               contracts::HttpQueryHandlerFn handler, void* ctx);

// Host endpoint-binder registration keeps the compatibility route label distinct from the
// descriptor lifecycle owner. These registrations remain hidden behind a pending tombstone
// until the binder commits the complete owner batch.
QolResult http_dispatch_register_query_for_owner(const char* lifecycle_owner,
                                                 contracts::HttpMethod method, const char* path,
                                                 const char* route_owner,
                                                 contracts::HttpQueryHandlerFn handler, void* ctx);
QolResult http_dispatch_register_command_for_owner(const char* lifecycle_owner,
                                                   contracts::HttpMethod method, const char* path,
                                                   const char* route_owner,
                                                   contracts::HttpCommandHandlerFn handler,
                                                   void* ctx);
void http_dispatch_set_owner_ready(const char* lifecycle_owner);
bool http_dispatch_rollback_uncommitted_owner(const char* lifecycle_owner);

// API/listener compatibility hook. API transport availability does not authorize OP routes.
void http_dispatch_set_api_enabled(bool enabled);

#if defined(QOL_HTTP_DISPATCH_TEST_SEAMS)
// Host-only tests inject the generic Host security decision without a second production authority.
void http_dispatch_set_op_authorizer_for_test(bool (*authorizer)());
// Compatibility adapter for older host-only dispatch fixtures.
void http_dispatch_set_op_enabled(bool enabled);
#endif

// host-only 测试使用；生产生命周期不绕过 dispatch API 调用此函数。
void http_dispatch_reset_for_test();

}  // namespace qol::host
