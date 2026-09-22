#pragma once

#include "contracts/http.h"

namespace qol::host {

// Host SDK 注入插件的 HTTP handler 接缝。
contracts::QolHttpDispatchApi http_dispatch_api();

// 插件描述符尾部的 query handler 声明由宿主生命周期调用。
QolResult http_dispatch_register_queries(const contracts::HttpQueryHandlerDescriptor* handlers,
                                         uint32_t count);

// 试点 JNI 使用的内置 handler 注册入口；业务插件仍通过 Host SDK 注册。
QolResult http_dispatch_register_builtin_query(contracts::HttpMethod method, const char* path,
                                               const char* owner,
                                               contracts::HttpQueryHandlerFn handler, void* ctx);

// host-only 测试使用；生产生命周期不绕过 dispatch API 调用此函数。
void http_dispatch_reset_for_test();

}  // namespace qol::host
