#pragma once

#include "contracts/plugin.h"

namespace qol::host {

using HttpEndpointOpAuthorizer = bool (*)();

bool http_endpoint_binder_validate_descriptor(const contracts::PluginDescriptor&);
bool http_endpoint_binder_register_owner(const contracts::PluginDescriptor&);
bool http_endpoint_binder_owner_enabled(const contracts::PluginDescriptor&);
bool http_endpoint_binder_owner_http_degraded(const char* lifecycle_owner);
void http_endpoint_binder_owner_stop_begin(const char* lifecycle_owner);
bool http_endpoint_binder_owner_stop_poll(const char* lifecycle_owner);
void http_endpoint_binder_set_provider_available(bool available);
bool http_endpoint_binder_provider_drain_poll();
bool http_endpoint_binder_all_stops_drained();
QolResult http_endpoint_binder_route_enter(contracts::HttpMethod method, const char* path,
                                           uint32_t path_len, HttpEndpointOpAuthorizer,
                                           uint64_t* out_lease);
QolResult http_endpoint_binder_route_leave(uint64_t lease);

void http_endpoint_binder_reset_for_test();

}  // namespace qol::host
