#pragma once

#include <cstdint>
#include <vector>

namespace qol::features::custom_map {

struct CustomMapSummary {
    uint32_t id;
    uint32_t width;
    uint32_t height;
};

struct CustomMapExit {
    uint8_t x;
    uint8_t y;
    uint8_t target_x;
    uint8_t target_y;
    uint16_t target_map_and_flags;
};

struct CustomMapDetail {
    uint32_t id;
    uint32_t width;
    uint32_t height;
    std::vector<uint16_t> base_tiles;
    std::vector<CustomMapExit> exits;
};

bool custom_map_scan(std::vector<CustomMapSummary>* out);
bool custom_map_read_detail(uint32_t id, CustomMapDetail* out);

}  // namespace qol::features::custom_map
