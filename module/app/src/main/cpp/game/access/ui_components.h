#pragma once

#include "game_ui_kit.h"

namespace ui_original {

struct ItemIcon {
    int32_t image_unit;
    int32_t image_loc;
    int32_t draw_type;
    int32_t flip;
    bool centered;
};

bool load_option_images();
void unload_option_images();
bool draw_back_button(void* ctrl, bool pressed);
bool draw_toggle(void* ctrl, bool enabled);
bool draw_item_icon(void* ctrl, const ItemIcon& icon);

}  // namespace ui_original

namespace ui_custom {

void draw_button(void* ctrl, UiRect size, uint32_t background, uint32_t border, int border_thickness,
                 const char* text, uint32_t text_color, int text_x, int text_y);

}  // namespace ui_custom
