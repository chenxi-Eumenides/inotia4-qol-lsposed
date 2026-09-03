#include "feature/extension_bag/extension_bag_geometry.h"

#include "feature/extension_bag/extension_bag_context.h"
#include "core/native/grid_geometry.h"
#include "game_access.h"
#include "game_symbols.h"

namespace {

constexpr GridGeometry kExtensionGrid{
    761, 160, 4, 4, 74, 74, 83, 83};

}

bool extension_bag_grid_hit(int64_t x, int64_t y) {
    const virtual_bag::State* state = extension_bag_state();
    if (state == nullptr || state->mode == virtual_bag::Mode::kOriginal ||
        !virtual_bag::valid_index(state->selected)) {
        return false;
    }
    return grid_contains(kExtensionGrid, x, y);
}

int extension_bag_grid_slot_index(int64_t x, int64_t y,
                                  int64_t origin_x, int64_t origin_y) {
    const GridGeometry grid{
        origin_x, origin_y, 4, 4, kExtensionGrid.cell_width,
        kExtensionGrid.cell_height, kExtensionGrid.step_x, kExtensionGrid.step_y};
    return grid_slot_index(grid, x, y);
}

void* extension_bag_valid_child(void* root, int slot) {
    if (root == nullptr || slot < 0 || fn_ctrl_get_count == nullptr ||
        fn_control_object_get_child == nullptr) {
        return nullptr;
    }
    if (static_cast<int>(fn_ctrl_get_count(root)) <= slot) return nullptr;
    void* ctrl = fn_control_object_get_child(root, static_cast<uint32_t>(slot));
    if (ctrl == nullptr || fn_control_object_get_user_type == nullptr ||
        fn_control_object_get_user_type(ctrl) != 2) {
        return nullptr;
    }
    return ctrl;
}

void extension_bag_control_abs_pos(void* button, int64_t* ax, int64_t* ay) {
    uint8_t* c = reinterpret_cast<uint8_t*>(button);
    *ax = *reinterpret_cast<int64_t*>(c + CO_RECT_X);
    *ay = *reinterpret_cast<int64_t*>(c + CO_RECT_Y);
    void* parent = *reinterpret_cast<void**>(c + CO_PARENT);
    while (parent != nullptr) {
        uint8_t* pc = reinterpret_cast<uint8_t*>(parent);
        *ax += *reinterpret_cast<int64_t*>(pc + CO_RECT_X);
        *ay += *reinterpret_cast<int64_t*>(pc + CO_RECT_Y);
        parent = *reinterpret_cast<void**>(pc + CO_PARENT);
    }
}
