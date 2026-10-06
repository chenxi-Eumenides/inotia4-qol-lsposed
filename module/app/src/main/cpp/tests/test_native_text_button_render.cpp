#include <algorithm>
#include <array>
#include <cassert>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <string>
#include <vector>

#include "game/access/game_access.h"
#include "game/access/ui_native_toggle.h"
#include "game/access/ui_native_text_button.h"

namespace {

struct DrawCall {
    std::array<int32_t, 12> args;
    void* group;
};

alignas(8) std::array<uint8_t, 0x18> g_location{};
alignas(8) std::array<uint8_t, 0x18> g_toggle_location{};
uint8_t g_group{};
uint8_t g_toggle_group{};
std::vector<DrawCall> g_draws;
std::string g_drawn_label;
std::array<int32_t, 4> g_clip{};
std::array<int32_t, 3> g_text_rgb{};
int32_t g_text_x = 0;
int32_t g_text_y = 0;
int32_t g_text_align = -1;
int32_t g_text_font = -1;
int g_font_height = 22;
int g_clip_calls = 0;
int g_reset_clip_calls = 0;
bool g_main_thread = true;
bool g_group_available = true;
bool g_location_available = true;
int g_loads = 0;

void* fake_group(int32_t unit) {
    assert(unit == 0x12 || unit == 0x54);
    if (!g_group_available) return nullptr;
    return unit == 0x12 ? static_cast<void*>(&g_group) : static_cast<void*>(&g_toggle_group);
}

void* fake_loc(int32_t unit, int32_t loc) {
    if (unit == 0x12 && loc == 0x01) {
        return g_location_available ? g_location.data() : nullptr;
    }
    if (unit == 0x54 && loc == 0x0f) return g_toggle_location.data();
    assert(false);
    return nullptr;
}

void fake_load(int32_t unit) {
    assert(unit == 0x12 || unit == 0x54);
    ++g_loads;
}

int32_t fake_title_image_unit() { return 0x54; }

void fake_draw(void* group, int32_t x, int32_t y, int32_t sx, int32_t sy, int32_t sw,
               int32_t sh, int32_t ox, int32_t oy, int32_t mode, int32_t alpha,
               int32_t percent, int32_t rotation) {
    g_draws.push_back({{x, y, sx, sy, sw, sh, ox, oy, mode, alpha, percent, rotation}, group});
}

uint64_t fake_font_height(uint32_t font) {
    assert(font <= 1);
    g_font_height = font == 0 ? 22 : 26;
    return static_cast<uint64_t>(g_font_height);
}

void fake_set_clip(int32_t x1, int32_t y1, int32_t x2, int32_t y2) {
    g_clip = {x1, y1, x2, y2};
    ++g_clip_calls;
}

void fake_reset_clip() { ++g_reset_clip_calls; }

void fake_set_font_color(int32_t red, int32_t green, int32_t blue) {
    g_text_rgb = {red, green, blue};
}

void fake_draw_string(char* text, int32_t x, int32_t y, int32_t align, int32_t font) {
    g_drawn_label = text;
    g_text_x = x;
    g_text_y = y;
    g_text_align = align;
    g_text_font = font;
}

void set_field(size_t offset, int32_t value) {
    std::memcpy(g_location.data() + offset, &value, sizeof(value));
}

void setup() {
    g_draws.clear();
    g_drawn_label.clear();
    g_clip = {};
    g_text_rgb = {};
    g_text_x = 0;
    g_text_y = 0;
    g_text_align = -1;
    g_text_font = -1;
    g_font_height = 22;
    g_clip_calls = 0;
    g_reset_clip_calls = 0;
    g_loads = 0;
    g_main_thread = true;
    g_group_available = true;
    g_location_available = true;
    set_field(IMAGE_LOC_SOURCE_X, 2);
    set_field(IMAGE_LOC_SOURCE_Y, 124);
    set_field(IMAGE_LOC_SOURCE_WIDTH, 470);
    set_field(IMAGE_LOC_SOURCE_HEIGHT, 112);
    set_field(IMAGE_LOC_OFFSET_X, 0);
    set_field(IMAGE_LOC_OFFSET_Y, 0);
    constexpr int32_t toggle_source_x = 2;
    constexpr int32_t toggle_source_y = 491;
    constexpr int32_t toggle_source_width = 246;
    constexpr int32_t toggle_source_height = 87;
    std::memcpy(g_toggle_location.data() + IMAGE_LOC_SOURCE_X, &toggle_source_x,
                sizeof(toggle_source_x));
    std::memcpy(g_toggle_location.data() + IMAGE_LOC_SOURCE_Y, &toggle_source_y,
                sizeof(toggle_source_y));
    std::memcpy(g_toggle_location.data() + IMAGE_LOC_SOURCE_WIDTH, &toggle_source_width,
                sizeof(toggle_source_width));
    std::memcpy(g_toggle_location.data() + IMAGE_LOC_SOURCE_HEIGHT, &toggle_source_height,
                sizeof(toggle_source_height));
    constexpr int32_t zero = 0;
    std::memcpy(g_toggle_location.data() + IMAGE_LOC_OFFSET_X, &zero, sizeof(zero));
    std::memcpy(g_toggle_location.data() + IMAGE_LOC_OFFSET_Y, &zero, sizeof(zero));
}

}  // namespace

decltype(fn_imgsys_unit_load) fn_imgsys_unit_load = &fake_load;
decltype(fn_imgsys_get_group) fn_imgsys_get_group = &fake_group;
decltype(fn_imgsys_get_loc) fn_imgsys_get_loc = &fake_loc;
decltype(fn_get_group_title_img_type) fn_get_group_title_img_type = &fake_title_image_unit;
decltype(fn_sgl_draw_texture_part_ex) fn_sgl_draw_texture_part_ex = &fake_draw;
decltype(fn_grpx_get_font_height) fn_grpx_get_font_height = &fake_font_height;
decltype(fn_grpx_set_clip) fn_grpx_set_clip = &fake_set_clip;
decltype(fn_grpx_reset_clip) fn_grpx_reset_clip = &fake_reset_clip;
decltype(fn_grpx_set_font_color_rgb) fn_grpx_set_font_color_rgb = &fake_set_font_color;
decltype(fn_grpx_draw_string_with_font) fn_grpx_draw_string_with_font = &fake_draw_string;

bool game_memory_accessible(const void* address, size_t, char) { return address != nullptr; }

namespace qol::game {
bool game_host_is_main_thread() { return g_main_thread; }
}

int main() {
    using qol::game::ui_draw_native_text_button_background;
    using qol::game::ui_draw_native_text_button_label;
    setup();
    assert(ui_draw_native_text_button_background({0, 0, 470, 112}, 0));
    assert(g_draws.size() == 1 && g_loads == 1);
    assert((g_draws[0].args == std::array<int32_t, 12>{0, 0, 2, 124, 470, 112, 0, 0,
                                                        1, 0, 100, 0}));
    assert(g_draws[0].group == &g_group);

    setup();
    assert(ui_draw_native_text_button_background({10, 20, 940, 224}, 1));
    assert(g_draws.size() == 1 && g_draws[0].args[10] == 200);
    assert(g_draws[0].args[8] == 1 && g_draws[0].args[9] == 0 && g_draws[0].args[11] == 0);

    setup();
    const UiRect full_button{0, 0, 220, 48};
    assert(ui_draw_native_text_button_background(full_button, 0));
    assert(g_draws.size() == 1);
    assert(ui_draw_native_text_button_label(full_button, "打开", 0x00112233u, 0));
    assert(g_drawn_label == "打开");
    assert(g_text_align == 2 && g_text_font == 0 && g_text_x == 110);
    // The fake native font paints [y, y + font_height); center its measured glyph box in the capsule.
    const int32_t drawn_image_width = g_draws[0].args[4] * g_draws[0].args[10] / 100;
    const int32_t drawn_image_height = g_draws[0].args[5] * g_draws[0].args[10] / 100;
    const int32_t capsule_left = g_draws[0].args[0] + drawn_image_width * 11 / 100;
    const int32_t capsule_right = g_draws[0].args[0] + drawn_image_width * 89 / 100;
    const int32_t capsule_top = g_draws[0].args[1] + drawn_image_height * 18 / 100;
    const int32_t capsule_bottom = g_draws[0].args[1] + drawn_image_height * 86 / 100;
    assert(g_text_y + g_font_height / 2 == capsule_top + (capsule_bottom - capsule_top) / 2);
    assert(capsule_left < g_text_x && g_text_x < capsule_right);
    assert(g_text_y >= capsule_top && g_text_y + g_font_height <= capsule_bottom);
    assert(g_clip_calls == 0 && g_reset_clip_calls == 0);
    assert((g_text_rgb == std::array<int32_t, 3>{0x33, 0x22, 0x11}));

    setup();
    const UiRect narrow_button{0, 0, 204, 48};
    assert(ui_draw_native_text_button_background(narrow_button, 0));
    assert(ui_draw_native_text_button_label(narrow_button, "打开", 0, 0));
    assert(g_text_y >= g_draws[0].args[1]);

    setup();
    const UiRect too_short_button{0, 0, 116, 24};
    assert(!ui_draw_native_text_button_label(too_short_button, "打开", 0, 0));
    assert(g_drawn_label.empty() && g_clip_calls == 0);

    setup();
    const char* long_label = "这是一个很长的UTF8按钮标签";
    assert(ui_draw_native_text_button_label(full_button, long_label, 0, 0));
    assert(g_drawn_label.size() < std::strlen(long_label));
    assert(g_drawn_label.size() >= std::strlen("…") &&
           g_drawn_label.compare(g_drawn_label.size() - std::strlen("…"), std::strlen("…"), "…") == 0);
    assert(g_drawn_label.size() / 3 <= 7);
    assert(g_text_x >= 0 && g_text_x <= full_button.w);
    assert(g_text_y >= 0 && g_text_y + g_font_height <= full_button.h);

    setup();
    const UiRect settings_toggle_rect{332, 8, 220, 80};
    assert(qol::game::ui_draw_native_toggle(settings_toggle_rect, 1, 0));
    assert(g_draws.size() == 1);
    const DrawCall native_toggle_call = g_draws.front();
    constexpr int32_t kOnBodyX = 13;
    constexpr int32_t kOnBodyY = 13;
    constexpr int32_t kOnBodyWidth = 220;
    constexpr int32_t kOnBodyHeight = 61;
    const int32_t toggle_percent = native_toggle_call.args[10];
    assert(native_toggle_call.args[2] == 2 && native_toggle_call.args[3] == 491 &&
           native_toggle_call.args[4] == 246 && native_toggle_call.args[5] == 87);
    const auto scale_round = [toggle_percent](int32_t value) {
        return (value * toggle_percent + 50) / 100;
    };
    const int32_t toggle_body_left = native_toggle_call.args[0] +
        scale_round(native_toggle_call.args[6] + kOnBodyX);
    const int32_t toggle_body_top = native_toggle_call.args[1] +
        scale_round(native_toggle_call.args[7] + kOnBodyY);
    const int32_t toggle_visible_width = scale_round(kOnBodyWidth);
    const int32_t toggle_visible_height = scale_round(kOnBodyHeight);
    g_draws.clear();
    assert(ui_draw_native_text_button_background(settings_toggle_rect, 0));
    assert(g_draws.size() == 3);
    assert(g_draws[0].args[10] == 70 && g_draws[1].args[10] == 70 &&
           g_draws[2].args[10] == 70);
    assert(g_draws[0].args[2] == 35 && g_draws[0].args[4] == 100);
    assert(g_draws[1].args[2] == 135 && g_draws[1].args[4] == 70);
    assert(g_draws[2].args[2] == 340 && g_draws[2].args[4] == 100);
    assert(g_draws[0].args[3] == 146 && g_draws[0].args[5] == 77);
    // The three slices must cover the full body source rect [35,440) x [146,223): the capsule
    // contour plus its black sampling guards, so no right/bottom edge pixel is dropped.
    assert(g_draws[0].args[2] == 35);
    assert(g_draws[2].args[2] + g_draws[2].args[4] == 440);
    for (size_t i = 0; i < g_draws.size(); ++i) {
        assert(g_draws[i].args[3] == 146 && g_draws[i].args[5] == 77);
    }
    assert(g_draws[0].args[0] + g_draws[0].args[4] * g_draws[0].args[10] / 100 ==
           g_draws[1].args[0]);
    assert(g_draws[1].args[0] + g_draws[1].args[4] * g_draws[1].args[10] / 100 ==
           g_draws[2].args[0]);
    int32_t gray_body_width = 0;
    int32_t gray_body_height = 0;
    int32_t gray_body_left = INT32_MAX;
    int32_t gray_body_top = INT32_MAX;
    int32_t gray_body_right = INT32_MIN;
    int32_t gray_body_bottom = INT32_MIN;
    double source_scaled_body_width = 0;
    for (size_t slice_index = 0; slice_index < g_draws.size(); ++slice_index) {
        const DrawCall& slice = g_draws[slice_index];
        const int32_t source_left = slice.args[2];
        const int32_t source_right = source_left + slice.args[4];
        const int32_t source_top = slice.args[3];
        const int32_t source_bottom = source_top + slice.args[5];
        const int32_t body_left = source_left > 35 ? source_left : 35;
        const int32_t body_right = source_right < 439 ? source_right : 439;
        const int32_t body_top = source_top > 146 ? source_top : 146;
        const int32_t body_bottom = source_bottom < 221 ? source_bottom : 221;
        assert(body_right > body_left && body_bottom > body_top);
        assert(source_left >= 2 && source_right <= 472);
        assert(source_top >= 124 && source_bottom <= 236);
        gray_body_width += (body_right - body_left) * slice.args[10] / 100;
        gray_body_height = (body_bottom - body_top) * slice.args[10] / 100;
        source_scaled_body_width += static_cast<double>(body_right - body_left) *
            slice.args[10] / 100.0;
        gray_body_left = std::min(gray_body_left, slice.args[0]);
        gray_body_top = std::min(gray_body_top, slice.args[1]);
        gray_body_right = std::max(gray_body_right,
            slice.args[0] + (slice.args[4] * slice.args[10] + 99) / 100);
        gray_body_bottom = std::max(gray_body_bottom,
            slice.args[1] + (slice.args[5] * slice.args[10] + 99) / 100);
        assert(slice.args[8] == 1 && slice.args[9] == 0 && slice.args[11] == 0);
        if (slice_index + 1 < g_draws.size()) {
            const DrawCall& next = g_draws[slice_index + 1];
            assert((slice.args[4] * slice.args[10]) % 100 == 0);
            assert(next.args[0] == slice.args[0] + slice.args[4] * slice.args[10] / 100);
        }
    }
    // UI_006.png.jpg has RGB (not alpha) pixels. Its gray capsule spans x=35..438, y=146..220;
    // x=439 and y=221..222 are black sampling guards within loc 0x01's source bounds.
    // Compare drawn capsule pixels to the toggle bounds while ensuring those outer pixels are
    // sampled. The capsule targets its own 212-logical body model (drawn 189 wide at 70%,
    // shifted +3 to stay centered) rather than the native toggle's 220 model, because integer
    // slice widths only allow 7-logical-px width steps (189 or 196).
    constexpr int32_t kTextButtonTargetWidth = 212;
    constexpr int32_t kTextButtonBodyShiftX = 3;
    const int32_t text_button_visible_width = scale_round(kTextButtonTargetWidth);
    const int32_t text_button_body_left = toggle_body_left + kTextButtonBodyShiftX;
    assert(text_button_visible_width == 189);
    assert(std::abs(gray_body_width - text_button_visible_width) <= 2);
    assert(std::abs(source_scaled_body_width - text_button_visible_width) <= 1.0);
    assert(std::abs(gray_body_height - toggle_visible_height) <= 2);
    assert(std::abs(gray_body_left - text_button_body_left) <= 1);
    assert(std::abs(gray_body_top - toggle_body_top) <= 1);
    assert(std::abs((gray_body_right - gray_body_left) - text_button_visible_width) <= 2);
    assert(std::abs((gray_body_bottom - gray_body_top) - toggle_visible_height) <= 2);
    // The narrower capsule must stay centered in the settings row: left/right margins
    // may differ by at most 1 logical px.
    const int32_t capsule_left_margin = gray_body_left - settings_toggle_rect.x;
    const int32_t capsule_right_margin =
        settings_toggle_rect.x + settings_toggle_rect.w - gray_body_right;
    assert(std::abs(capsule_left_margin - capsule_right_margin) <= 1);
    assert(ui_draw_native_text_button_label(settings_toggle_rect, "打开", 0, 1));
    assert(g_text_font == 1 && g_font_height == 26);
    const int32_t toggle_center_x = toggle_body_left + toggle_visible_width / 2;
    const int32_t toggle_center_y = toggle_body_top + toggle_visible_height / 2;
    const int32_t text_button_center_x = text_button_body_left + text_button_visible_width / 2;
    assert(std::abs(g_text_x - toggle_center_x) <= 1);
    // The label must also sit centered on the narrowed capsule itself.
    assert(std::abs(g_text_x - text_button_center_x) <= 1);
    assert(std::abs(g_text_y + g_font_height / 2 - toggle_center_y) <= 1);

    setup();
    assert(!ui_draw_native_text_button_label(full_button, "\xf0\x28\x8c\x28", 0, 0));
    assert(g_clip_calls == 0 && g_drawn_label.empty());

    setup();
    assert(ui_draw_native_text_button_background({0, 0, 470, 140}, 0));
    assert(g_draws[0].args[0] == 0 && g_draws[0].args[1] == 14);

    setup();
    g_group_available = false;
    assert(!ui_draw_native_text_button_background({0, 0, 470, 112}, 0) && g_draws.empty());
    setup();
    g_location_available = false;
    assert(!ui_draw_native_text_button_background({0, 0, 470, 112}, 0) && g_draws.empty());
    setup();
    g_main_thread = false;
    assert(!ui_draw_native_text_button_background({0, 0, 470, 112}, 0) && g_draws.empty());
    setup();
    assert(!ui_draw_native_text_button_background({0, 0, 0, 112}, 0));
    assert(!ui_draw_native_text_button_background({std::numeric_limits<int32_t>::max(), 0, 1, 112}, 0));
    assert(!ui_draw_native_text_button_background({0, 0, 470, 112}, 2));
    assert(g_draws.empty());
    return 0;
}
