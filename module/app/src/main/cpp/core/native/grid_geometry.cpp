#include "core/native/grid_geometry.h"

bool grid_contains(const GridGeometry& grid, int64_t x, int64_t y) {
    if (grid.columns <= 0 || grid.rows <= 0 || grid.cell_width <= 0 ||
        grid.cell_height <= 0 || grid.step_x <= 0 || grid.step_y <= 0) {
        return false;
    }
    const int64_t width = grid.columns * grid.step_x - (grid.step_x - grid.cell_width);
    const int64_t height = grid.rows * grid.step_y - (grid.step_y - grid.cell_height);
    return x >= grid.origin_x && x < grid.origin_x + width &&
           y >= grid.origin_y && y < grid.origin_y + height;
}

int grid_slot_index(const GridGeometry& grid, int64_t x, int64_t y) {
    if (!grid_contains(grid, x, y)) return -1;
    const int64_t relative_x = x - grid.origin_x;
    const int64_t relative_y = y - grid.origin_y;
    const int column = static_cast<int>(relative_x / grid.step_x);
    const int row = static_cast<int>(relative_y / grid.step_y);
    if (relative_x % grid.step_x >= grid.cell_width ||
        relative_y % grid.step_y >= grid.cell_height) {
        return -1;
    }
    return row * grid.columns + column;
}
