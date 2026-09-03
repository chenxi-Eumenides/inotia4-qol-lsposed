#pragma once

#include <cstdint>

struct GridGeometry {
    int64_t origin_x = 0;
    int64_t origin_y = 0;
    int columns = 0;
    int rows = 0;
    int64_t cell_width = 0;
    int64_t cell_height = 0;
    int64_t step_x = 0;
    int64_t step_y = 0;
};

bool grid_contains(const GridGeometry& grid, int64_t x, int64_t y);
int grid_slot_index(const GridGeometry& grid, int64_t x, int64_t y);
