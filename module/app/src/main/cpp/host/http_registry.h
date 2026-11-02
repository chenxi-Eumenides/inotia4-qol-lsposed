#pragma once

#include <cstdint>
#include <string>

#include "contracts/http.h"

namespace qol::host {

struct HttpRouteMatch {
    contracts::HttpRoute route{};
    const char* lifecycle_owner = nullptr;
    bool resident = false;
    bool binder_managed = false;
    bool unregister_pending = false;
};

// core.api 注入 Host SDK 的 HTTP 注册表函数表。
contracts::QolHttpRegistryApi http_registry_api();

// /api/system/routes 的 native 诊断数据源。
std::string host_http_routes_json();

// dispatch 在查找 handler 前使用，避免 inactive 状态返回普通 404。
bool host_http_registry_accepting();

// Host-internal endpoint binder seams. route.owner remains the public label; lifecycle_owner
// is the descriptor ID used for teardown. Resident routes have no plugin lifecycle owner.
QolResult http_registry_register_route_for_owner(const char* lifecycle_owner,
                                                const contracts::HttpRoute&, bool resident);
void http_registry_set_owner_ready(const char* lifecycle_owner);
void http_registry_begin_unregister_lifecycle_owner(const char* lifecycle_owner);
bool http_registry_poll_unregister_lifecycle_owner(const char* lifecycle_owner);
bool http_registry_rollback_uncommitted_owner(const char* lifecycle_owner);
bool http_registry_poll_nonresident_leases();
void http_registry_set_provider_available(bool available);
QolResult http_registry_find_match(contracts::HttpMethod, const char* actual_path,
                                   uint32_t actual_path_len, HttpRouteMatch* out_match);
QolResult http_registry_lease_enter_match(contracts::HttpMethod, const char* actual_path,
                                          uint32_t actual_path_len, uint64_t* out_lease);
QolResult http_registry_lease_leave(uint64_t lease);

// host-only 测试使用；生产生命周期不绕过 core.api 调用此函数。
void http_registry_reset_for_test();

}  // namespace qol::host
