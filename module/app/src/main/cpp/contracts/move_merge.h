#pragma once

#include <cstdint>
#include <type_traits>

#include "item_handle.h"
#include "result.h"

namespace qol::contracts {

constexpr const char* kMoveMergeControlCapabilityId = "move_merge.control";
constexpr uint16_t kMoveMergeControlVersionMajor = 1;
constexpr uint16_t kMoveMergeControlVersionMinor = 0;

struct MoveMergeControlApi {
    uint32_t struct_size;
    uint16_t version_major;
    uint16_t version_minor;
    bool (*enabled)();
};

static_assert(std::is_standard_layout<MoveMergeControlApi>::value,
              "move merge control API must be standard layout");
static_assert(std::is_trivially_copyable<MoveMergeControlApi>::value,
              "move merge control API must be trivially copyable");

inline bool move_merge_control_api_valid(const MoveMergeControlApi* api) {
    return api != nullptr && api->struct_size >= sizeof(MoveMergeControlApi) &&
           api->version_major == kMoveMergeControlVersionMajor &&
           api->version_minor >= kMoveMergeControlVersionMinor;
}

}  // namespace qol::contracts
