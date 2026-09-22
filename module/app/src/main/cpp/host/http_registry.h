#pragma once

#include <cstdint>
#include <string>

#include "contracts/http.h"

namespace qol::host {

// core.api 注入 Host SDK 的 HTTP 注册表函数表。
contracts::QolHttpRegistryApi http_registry_api();

// /api/system/routes 的 native 诊断数据源。
std::string host_http_routes_json();

// dispatch 在查找 handler 前使用，避免 inactive 状态返回普通 404。
bool host_http_registry_accepting();

// host-only 测试使用；生产生命周期不绕过 core.api 调用此函数。
void http_registry_reset_for_test();

}  // namespace qol::host
