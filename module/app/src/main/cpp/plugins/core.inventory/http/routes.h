#pragma once

#include <cstdint>

#include "contracts/plugin.h"

namespace qol::features::core_inventory {

// inventory HTTP 读侧 endpoint 表；descriptor 生命周期为进程静态存储。
void initialize_http_endpoints();
extern contracts::HttpEndpointDescriptor kHttpEndpoints[];
extern const uint32_t kHttpEndpointCount;

}  // namespace qol::features::core_inventory
