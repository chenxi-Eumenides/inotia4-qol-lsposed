#pragma once

#include "contracts/plugin.h"

namespace qol::features::extension_bag {

inline constexpr uint32_t kEbResultHandlerId = 0x45420001;
inline constexpr uint32_t kHttpEndpointCount = 8;

extern const qol::contracts::HttpEndpointDescriptor kHttpEndpoints[kHttpEndpointCount];

inline constexpr const qol::contracts::HttpEndpointDescriptor*
extension_bag_http_endpoints() {
    return kHttpEndpoints;
}

const qol::contracts::HttpEndpointDescriptor* extension_bag_http_endpoints(uint32_t* out_count);
const qol::contracts::QolHostSdk* extension_bag_http_host_sdk();
bool eb_result_handler(void* ctx, const uint8_t* payload, uint32_t size, char* out,
                       uint32_t capacity, uint32_t* out_size);

}  // namespace qol::features::extension_bag
