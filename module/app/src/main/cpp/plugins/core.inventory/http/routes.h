#pragma once

#include <cstdint>

#include "contracts/plugin.h"

namespace qol::features::core_inventory {

// inventory HTTP endpoint 表：读侧 5 条 GET + 写侧 2 条 POST；
// descriptor 生命周期为进程静态存储。
void initialize_http_endpoints();
extern contracts::HttpEndpointDescriptor kHttpEndpoints[];
extern const uint32_t kHttpEndpointCount;
extern const contracts::HttpRouteMetadataDescriptor kHttpRouteMetadata[];
extern const uint32_t kHttpRouteMetadataCount;

}  // namespace qol::features::core_inventory
