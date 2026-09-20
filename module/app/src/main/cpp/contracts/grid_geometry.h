#pragma once

#include <cstdint>

namespace qol::contracts {

// 这是网格坐标公式，不是扩展背包的策略状态；调用方负责提供网格参数。
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

inline bool grid_contains(const GridGeometry& grid, int64_t x, int64_t y) {
    if (grid.columns <= 0 || grid.rows <= 0 || grid.cell_width <= 0 || grid.cell_height <= 0 ||
        grid.step_x <= 0 || grid.step_y <= 0) {
        return false;
    }
    const int64_t width = static_cast<int64_t>(grid.columns - 1) * grid.step_x + grid.cell_width;
    const int64_t height = static_cast<int64_t>(grid.rows - 1) * grid.step_y + grid.cell_height;
    return x >= grid.origin_x && y >= grid.origin_y && x < grid.origin_x + width &&
           y < grid.origin_y + height;
}

inline int grid_slot_index(const GridGeometry& grid, int64_t x, int64_t y) {
    if (!grid_contains(grid, x, y)) return -1;
    const int64_t column = (x - grid.origin_x) / grid.step_x;
    const int64_t row = (y - grid.origin_y) / grid.step_y;
    if (column < 0 || column >= grid.columns || row < 0 || row >= grid.rows) return -1;
    const int64_t cell_x = grid.origin_x + column * grid.step_x;
    const int64_t cell_y = grid.origin_y + row * grid.step_y;
    if (x >= cell_x + grid.cell_width || y >= cell_y + grid.cell_height) return -1;
    return static_cast<int>(row * grid.columns + column);
}

}  // namespace qol::contracts
