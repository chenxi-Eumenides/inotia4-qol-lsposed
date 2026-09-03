#include "game_ui_components.h"

#include "game_symbols.h"

namespace {

constexpr int32_t kOptionImageUnit = 0x59;
constexpr int32_t kBackPressedLoc = 0x2;
constexpr int32_t kBackNormalLoc = 0x3;
constexpr int32_t kToggleDimLoc = 0x9;
constexpr int32_t kToggleOnLoc = 0x0f;
constexpr int32_t kToggleOffLoc = 0x10;

bool control_center(void* ctrl, int32_t* x, int32_t* y) {
    if (ctrl == nullptr || x == nullptr || y == nullptr) return false;
    UiRect rect{};
    if (!ui_get_rect(ctrl, &rect)) return false;
    int64_t absolute_x = rect.x + rect.w / 2;
    int64_t absolute_y = rect.y + rect.h / 2;
    for (void* parent = *reinterpret_cast<void**>(static_cast<uint8_t*>(ctrl) + CO_PARENT);
         parent != nullptr;
         parent = *reinterpret_cast<void**>(static_cast<uint8_t*>(parent) + CO_PARENT)) {
        UiRect parent_rect{};
        if (!ui_get_rect(parent, &parent_rect)) return false;
        absolute_x += parent_rect.x;
        absolute_y += parent_rect.y;
    }
    *x = static_cast<int32_t>(absolute_x);
    *y = static_cast<int32_t>(absolute_y);
    return true;
}

}  // namespace

namespace ui_original {

bool load_option_images() {
    return ui_load_image_unit(kOptionImageUnit);
}

void unload_option_images() {
    ui_unload_image_unit(kOptionImageUnit);
}

bool draw_back_button(void* ctrl, bool pressed) {
    int32_t center_x = 0;
    int32_t center_y = 0;
    if (!control_center(ctrl, &center_x, &center_y)) return false;
    if (!ui_draw_image_part(kOptionImageUnit, kBackNormalLoc, center_x, center_y, 2, 1)) {
        return false;
    }
    if (pressed && !ui_draw_image_part(kOptionImageUnit, kBackPressedLoc,
                                       center_x - 3, center_y, 2, 1)) {
        return false;
    }
    return true;
}

bool draw_toggle(void* ctrl, bool enabled) {
    const int32_t loc = enabled ? kToggleOnLoc : kToggleOffLoc;
    if (!ui_draw_control_title_image_part(ctrl, loc)) return false;
    if (!enabled) ui_draw_control_title_image_part(ctrl, kToggleDimLoc);
    return true;
}

bool draw_item_icon(void* ctrl, const ItemIcon& icon) {
    if (icon.centered) {
        return ui_draw_control_image_part_centered(ctrl, icon.image_unit, icon.image_loc,
                                                   icon.draw_type, icon.flip);
    }
    return ui_draw_control_image_part(ctrl, icon.image_unit, icon.image_loc,
                                      icon.draw_type, icon.flip);
}

}  // namespace ui_original

namespace ui_custom {

void draw_button(void* ctrl, UiRect size, uint32_t background, uint32_t border, int border_thickness,
                 const char* text, uint32_t text_color, int text_x, int text_y) {
    ui_draw_button_background(ctrl, size, background);
    ui_draw_button_border(ctrl, size, border, border_thickness);
    if (text != nullptr) ui_draw_text(ctrl, text_x, text_y, text_color);
}

}  // namespace ui_custom
