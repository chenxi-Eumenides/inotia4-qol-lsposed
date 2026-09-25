#pragma once

#include <cstdint>

#include "result.h"
#include "version.h"

namespace qol::contracts {

constexpr const char* kStaticDataCapabilityId = "static.data";
constexpr QolVersion kStaticDataVersion{1, 0};

struct QolStaticMapExit {
    int32_t x;
    int32_t y;
    int32_t target_map_id;
    int32_t target_x;
    int32_t target_y;
};

struct QolStaticDataApi {
    QolResult (*map_name)(int32_t map_id, char* out, uint32_t capacity, uint32_t* out_size);
    QolResult (*map_exits)(int32_t map_id, QolStaticMapExit* out, uint32_t capacity,
                           uint32_t* out_count);
};

}  // namespace qol::contracts
