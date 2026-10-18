#include "plugins/extension_bag/extension_bag_geometry.h"

#include "plugins/extension_bag/extension_bag_context.h"
#include "plugins/extension_bag/extension_bag_bridge.h"
#include "contracts/grid_geometry.h"
#include "contracts/ui.h"

#include <cstddef>

namespace {

const qol::contracts::QolUiAdapter* extension_bag_ui() {
    const auto* sdk = qol::features::extension_bag::extension_bag_host_sdk();
    return sdk == nullptr ? nullptr : sdk->ui;
}

constexpr qol::contracts::GridGeometry kExtensionGrid{
    761, 160, 4, 4, 74, 74, 83, 83};

}

bool extension_bag_grid_hit(int64_t x, int64_t y) {
    const virtual_bag::State* state = extension_bag_state();
    if (state == nullptr || state->mode == virtual_bag::Mode::kOriginal ||
        !virtual_bag::valid_index(state->selected)) {
        return false;
    }
    return qol::contracts::grid_contains(kExtensionGrid, x, y);
}

int extension_bag_grid_slot_index(int64_t x, int64_t y,
                                  int64_t origin_x, int64_t origin_y) {
    const qol::contracts::GridGeometry grid{
        origin_x, origin_y, 4, 4, kExtensionGrid.cell_width,
        kExtensionGrid.cell_height, kExtensionGrid.step_x, kExtensionGrid.step_y};
    return qol::contracts::grid_slot_index(grid, x, y);
}

void* extension_bag_valid_child(void* root, int slot) {
    if (root == nullptr || slot < 0) {
        return nullptr;
    }
    int count = 0;
    if (!extension_bag_ui_child_count(root, &count) || count <= slot) return nullptr;
    void* ctrl = extension_bag_ui_child_at(root, slot);
    if (ctrl == nullptr || extension_bag_ui_user_type(ctrl) != 2) {
        return nullptr;
    }
    return ctrl;
}

void extension_bag_control_abs_pos(void* button, int64_t* ax, int64_t* ay) {
    if (ax == nullptr || ay == nullptr || !extension_bag_ui_control_origin(button, ax, ay)) {
        if (ax != nullptr) *ax = 0;
        if (ay != nullptr) *ay = 0;
    }
}

bool extension_bag_ui_child_count(void* root, int* out_count) {
    if (out_count == nullptr) return false;
    *out_count = 0;
    const auto* ui = extension_bag_ui();
    if (ui == nullptr || ui->child_count == nullptr) return false;
    uint32_t count = 0;
    if (!qol::contracts::succeeded(ui->child_count(root, &count))) return false;
    *out_count = static_cast<int>(count);
    return true;
}

void* extension_bag_ui_child_at(void* root, int slot) {
    const auto* ui = extension_bag_ui();
    if (ui == nullptr || ui->child_at == nullptr || root == nullptr || slot < 0) return nullptr;
    void* child = nullptr;
    if (!qol::contracts::succeeded(ui->child_at(root, static_cast<uint32_t>(slot), &child))) return nullptr;
    return child;
}

int extension_bag_ui_user_type(void* control) {
    const auto* ui = extension_bag_ui();
    if (ui == nullptr || ui->control_user_type == nullptr || control == nullptr) return -1;
    int32_t type = -1;
    return qol::contracts::succeeded(ui->control_user_type(control, &type)) ? type : -1;
}

void* extension_bag_ui_item(void* control) {
    const auto* ui = extension_bag_ui();
    if (ui == nullptr || ui->control_item == nullptr || control == nullptr) return nullptr;
    void* item = nullptr;
    return qol::contracts::succeeded(ui->control_item(control, &item)) ? item : nullptr;
}

void* extension_bag_ui_data(void* control) {
    const auto* ui = extension_bag_ui();
    if (ui == nullptr || ui->control_data == nullptr || control == nullptr) return nullptr;
    void* data = nullptr;
    return qol::contracts::succeeded(ui->control_data(control, &data)) ? data : nullptr;
}

int extension_bag_ui_item_slot(void* control) {
    const auto* ui = extension_bag_ui();
    if (ui == nullptr || ui->control_item_slot == nullptr || control == nullptr) return -1;
    int32_t slot = -1;
    return qol::contracts::succeeded(ui->control_item_slot(control, &slot)) ? slot : -1;
}

void* extension_bag_ui_parent(void* control) {
    const auto* ui = extension_bag_ui();
    if (ui == nullptr || ui->control_parent == nullptr || control == nullptr) return nullptr;
    void* parent = nullptr;
    return qol::contracts::succeeded(ui->control_parent(control, &parent)) ? parent : nullptr;
}

void* extension_bag_ui_root(qol::contracts::QolUiRootKind kind) {
    const auto* ui = extension_bag_ui();
    if (ui == nullptr || ui->find_root == nullptr) return nullptr;
    void* root = nullptr;
    return qol::contracts::succeeded(ui->find_root(kind, &root)) ? root : nullptr;
}

bool extension_bag_ui_rect(void* control, UiRect* out_rect) {
    const auto* ui = extension_bag_ui();
    return ui != nullptr && ui->get_rect != nullptr && control != nullptr && out_rect != nullptr &&
           qol::contracts::succeeded(ui->get_rect(control, out_rect));
}

bool extension_bag_ui_set_rect(void* control, UiRect rect) {
    const auto* ui = extension_bag_ui();
    return ui != nullptr && ui->set_rect != nullptr && control != nullptr &&
           qol::contracts::succeeded(ui->set_rect(control, rect));
}

bool extension_bag_ui_control_origin(void* control, int64_t* out_x, int64_t* out_y) {
    const auto* ui = extension_bag_ui();
    return ui != nullptr && ui->control_absolute_origin != nullptr && control != nullptr &&
           out_x != nullptr && out_y != nullptr &&
           qol::contracts::succeeded(ui->control_absolute_origin(control, out_x, out_y));
}

bool extension_bag_ui_hit_test(void* control, int64_t x, int64_t y, UiRect rect) {
    const auto* ui = extension_bag_ui();
    return ui != nullptr && ui->hit_test != nullptr && control != nullptr &&
           ui->hit_test(control, x, y, rect) == 1;
}

bool extension_bag_ui_touch_state(void** out_moving, void** out_drop_source,
                                  int64_t* out_x, int64_t* out_y) {
    const auto* ui = extension_bag_ui();
    return ui != nullptr && ui->touch_state != nullptr && out_moving != nullptr &&
           out_drop_source != nullptr && out_x != nullptr && out_y != nullptr &&
           qol::contracts::succeeded(ui->touch_state(out_moving, out_drop_source, out_x, out_y));
}

bool extension_bag_ui_touch_coordinates(uint64_t param, int64_t* out_x, int64_t* out_y) {
    const auto* ui = extension_bag_ui();
    return ui != nullptr && ui->touch_coordinates != nullptr && out_x != nullptr && out_y != nullptr &&
           qol::contracts::succeeded(ui->touch_coordinates(param, out_x, out_y));
}

bool extension_bag_ui_draw_part(int32_t unit, int32_t loc, int32_t x, int32_t y,
                                int32_t type, int32_t flip) {
    const auto* ui = extension_bag_ui();
    if (ui == nullptr || ui->image_group == nullptr || ui->image_loc == nullptr ||
        ui->draw_part == nullptr) return false;
    void* group = nullptr;
    void* image = nullptr;
    if (!qol::contracts::succeeded(ui->image_group(unit, &group)) ||
        !qol::contracts::succeeded(ui->image_loc(unit, loc, &image))) return false;
    return qol::contracts::succeeded(ui->draw_part(group, x, y, image, type, flip));
}

bool extension_bag_ui_draw_part_alpha(int32_t unit, int32_t loc, int32_t x, int32_t y,
                                      int32_t type, int32_t flip, int32_t alpha) {
    const auto* ui = extension_bag_ui();
    constexpr std::size_t kDrawPartAlphaEnd =
        offsetof(qol::contracts::QolUiAdapter, draw_part_alpha) +
        sizeof(((qol::contracts::QolUiAdapter*)nullptr)->draw_part_alpha);
    if (ui == nullptr || ui->size < kDrawPartAlphaEnd || ui->draw_part_alpha == nullptr ||
        ui->image_group == nullptr || ui->image_loc == nullptr) return false;
    void* group = nullptr;
    void* image = nullptr;
    if (!qol::contracts::succeeded(ui->image_group(unit, &group)) ||
        !qol::contracts::succeeded(ui->image_loc(unit, loc, &image))) return false;
    return qol::contracts::succeeded(
        ui->draw_part_alpha(group, x, y, image, type, flip, alpha));
}

bool extension_bag_ui_can_draw() {
    const auto* ui = extension_bag_ui();
    return ui != nullptr && ui->image_group != nullptr && ui->image_loc != nullptr &&
           ui->draw_part != nullptr;
}

bool extension_bag_ui_draw_item(void* item, int32_t x, int32_t y, int32_t a, int32_t b) {
    const auto* ui = extension_bag_ui();
    return ui != nullptr && ui->draw_item_porting != nullptr &&
           qol::contracts::succeeded(ui->draw_item_porting(item, x, y, a, b));
}

bool extension_bag_ui_create_control(void* parent, void** out_control) {
    const auto* ui = extension_bag_ui();
    return ui != nullptr && ui->create_control != nullptr && out_control != nullptr &&
           qol::contracts::succeeded(ui->create_control(1, parent, out_control));
}

bool extension_bag_ui_delete_control(void* control) {
    const auto* ui = extension_bag_ui();
    return ui != nullptr && ui->delete_control != nullptr && control != nullptr &&
           qol::contracts::succeeded(ui->delete_control(control));
}

bool extension_bag_ui_set_control_item(void* control, void* item) {
    const auto* ui = extension_bag_ui();
    return ui != nullptr && ui->set_control_item != nullptr && control != nullptr &&
           qol::contracts::succeeded(ui->set_control_item(control, item));
}

bool extension_bag_ui_set_control_proc(void* control, qol::contracts::QolUiControlProcFn proc) {
    const auto* ui = extension_bag_ui();
    return ui != nullptr && ui->set_control_proc != nullptr && control != nullptr && proc != nullptr &&
           qol::contracts::succeeded(ui->set_control_proc(control, proc));
}

bool extension_bag_ui_set_control_user_type(void* control, int type) {
    const auto* ui = extension_bag_ui();
    return ui != nullptr && ui->set_control_user_type != nullptr && control != nullptr &&
           qol::contracts::succeeded(ui->set_control_user_type(control, type));
}

bool extension_bag_ui_set_control_data(void* control, void* data) {
    const auto* ui = extension_bag_ui();
    return ui != nullptr && ui->set_control_data != nullptr && control != nullptr &&
           qol::contracts::succeeded(ui->set_control_data(control, data));
}

bool extension_bag_ui_allocate_control_data(uint32_t size, void** out_data) {
    const auto* ui = extension_bag_ui();
    return ui != nullptr && ui->allocate_control_data != nullptr && out_data != nullptr &&
           qol::contracts::succeeded(ui->allocate_control_data(size, out_data));
}

bool extension_bag_ui_unuse_control_event_move(void* control) {
    const auto* ui = extension_bag_ui();
    return ui != nullptr && ui->unuse_control_event_move != nullptr && control != nullptr &&
           qol::contracts::succeeded(ui->unuse_control_event_move(control));
}

bool extension_bag_ui_control_is_moving(void* control) {
    const auto* ui = extension_bag_ui();
    if (ui == nullptr || ui->control_is_moving == nullptr || control == nullptr) return false;
    int32_t moving = 0;
    return qol::contracts::succeeded(ui->control_is_moving(control, &moving)) && moving != 0;
}

bool extension_bag_ui_clear_control_drag_flags(void* control) {
    const auto* ui = extension_bag_ui();
    return ui != nullptr && ui->clear_control_drag_flags != nullptr && control != nullptr &&
           qol::contracts::succeeded(ui->clear_control_drag_flags(control));
}

bool extension_bag_ui_clear_touch_state() {
    const auto* ui = extension_bag_ui();
    return ui != nullptr && ui->clear_touch_state != nullptr &&
           qol::contracts::succeeded(ui->clear_touch_state());
}

bool extension_bag_ui_set_touch_release_input(int64_t x, int64_t y, uint64_t param) {
    const auto* ui = extension_bag_ui();
    return ui != nullptr && ui->set_touch_release_input != nullptr &&
           qol::contracts::succeeded(ui->set_touch_release_input(x, y, param));
}

bool extension_bag_ui_clear_touch_drop_event() {
    const auto* ui = extension_bag_ui();
    return ui != nullptr && ui->clear_touch_drop_event != nullptr &&
           qol::contracts::succeeded(ui->clear_touch_drop_event());
}

bool extension_bag_ui_fill_rect(UiRect rect, uint32_t color) {
    const auto* ui = extension_bag_ui();
    return ui != nullptr && ui->fill_rect != nullptr &&
           qol::contracts::succeeded(ui->fill_rect(rect, color));
}
