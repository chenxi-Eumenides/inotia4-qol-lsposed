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

#include "fixtures/native_text_button_pressed_rgba.inc"

PressedRgba fixture_pixel(int32_t x, int32_t y) {
    if (x >= 99 && x <= 120 && y >= 39 && y <= 60) {
        const char* row = kPressedRgbaGuardRows[static_cast<size_t>(y - 39)];
        assert(std::strlen(row) == 22);
        const int32_t index = row[x - 99] - '0';
        assert(index >= 0 && index < static_cast<int32_t>(kPressedRgbaPalette.size()));
        return kPressedRgbaPalette[static_cast<size_t>(index)];
    }
    assert(y == 51 && x >= 29 && x <= 50);
    return kPressedEdgeGuardRow[static_cast<size_t>(x - 29)];
}

bool horizontally_uniform_rgba_with_guard(int32_t x, int32_t width, int32_t y) {
    const PressedRgba first = fixture_pixel(x - 1, y);
    for (int32_t sample_x = x; sample_x <= x + width; ++sample_x) {
        if (fixture_pixel(sample_x, y) != first) return false;
    }
    return true;
}

void test_pressed_rgba_fixture() {
    assert(std::strlen(kPressedRgbaPngSha256) == 64);
    for (int32_t y = 39; y <= 60; ++y) {
        assert(horizontally_uniform_rgba_with_guard(100, 20, y));
    }
    // A strip can have uniform RGB while repeating an unwanted alpha ramp.
    for (const auto& pixel : kPressedEdgeGuardRow) {
        assert(pixel[0] == 255 && pixel[1] == 157 && pixel[2] == 0);
    }
    assert(!horizontally_uniform_rgba_with_guard(30, 20, 51));
    assert(fixture_pixel(30, 51)[3] - fixture_pixel(49, 51)[3] == 69);
    const PressedRgba transparent_white{255, 255, 255, 0};
    assert(fixture_pixel(100, 51) == transparent_white);
    int32_t alpha_sum = 0;
    uint8_t min_alpha = 255;
    uint8_t max_alpha = 0;
    for (int32_t y = 40; y < 60; ++y) {
        for (int32_t x = 100; x < 120; ++x) {
            const auto pixel = fixture_pixel(x, y);
            min_alpha = std::min(min_alpha, pixel[3]);
            max_alpha = std::max(max_alpha, pixel[3]);
            alpha_sum += pixel[3];
            if (pixel[0] == 255 && pixel[1] == 255 && pixel[2] == 255) assert(pixel[3] == 0);
        }
    }
    assert(min_alpha == 0 && max_alpha == 10 && alpha_sum == 920);
}

void assert_middle_strip_rgba(const DrawCall& draw) {
    const auto& a = draw.args;
    assert(a[2] == 678 && a[3] == 232 && a[4] > 0 && a[4] <= 20 && a[5] == 20);
    // Check the actual selected source rectangle, including bilinear neighbors,
    // against independent PNG samples rather than the production source constant.
    for (int32_t y = a[3] - 192 - 1; y <= a[3] - 192 + a[5]; ++y) {
        assert(horizontally_uniform_rgba_with_guard(a[2] - 578, a[4], y));
        const auto pixel = fixture_pixel(a[2] - 578, y);
        assert(pixel[3] <= 13);
        if (pixel[0] == 255 && pixel[1] == 255 && pixel[2] == 255) assert(pixel[3] == 0);
    }
}

alignas(8) std::array<uint8_t, 0x18> g_location{};
alignas(8) std::array<uint8_t, 0x18> g_toggle_location{};
alignas(8) std::array<int32_t, 6> g_press_location{578, 192, 262, 103, 0, 0};
bool g_press_available = true;
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
    if (unit == 0x54 && loc == 0x7) return g_press_available ? g_press_location.data() : nullptr;
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
    constexpr int heights[] = {22, 26, 40, 30, 16};
    assert(font < 5);
    g_font_height = heights[font];
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
    g_press_available = true;
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

void assert_visible_press_contour(const std::vector<DrawCall>& normal,
                                  const std::vector<DrawCall>& overlays) {
    assert(!normal.empty() && overlays.size() >= 2);
    const auto& base = normal.front().args;
    const int32_t scale = base[10];
    // Independent artwork measurements, not the renderer's halo canvas:
    // normal UI_006 capsule alpha>128: x[33,438), y[22,97) relative to loc 1;
    // pressed title loc 7 alpha>128: x[21,241), y[21,82).
    const bool sliced = normal.size() == 3;
    const int32_t normal_left = base[0] + (sliced ? 0 : 33 * scale / 100);
    const int32_t normal_top = base[1] + (sliced ? 0 : 22 * scale / 100);
    int32_t normal_width = 405 * scale / 100;
    if (sliced) {
        normal_width = 0;
        for (const auto& slice : normal) normal_width += slice.args[4] * scale / 100;
    }
    const int32_t normal_height = 75 * scale / 100;
    const auto& left = overlays[0].args;
    const auto& right = overlays[1].args;
    const int32_t pressed_left = left[0] + 21 * left[10] / 100;
    const int32_t pressed_right = right[0] + 29 * right[10] / 100;
    const int32_t pressed_top = left[1] + 21 * left[10] / 100;
    const int32_t pressed_bottom = left[1] + 82 * left[10] / 100;
    assert(std::abs(pressed_left - normal_left) <= 2);
    assert(std::abs(pressed_right - normal_left - normal_width) <= 2);
    assert(std::abs(pressed_top - normal_top) <= 2);
    assert(std::abs(pressed_bottom - normal_top - normal_height) <= 2);
    assert(std::abs((pressed_right - pressed_left) - normal_width) <= 2);
    assert(std::abs((pressed_bottom - pressed_top) - normal_height) <= 2);
    // Preserve caps/top/bottom source slices; only the middle strip changes.
    size_t middle_slices = 0;
    for (const auto& overlay : overlays) {
        const auto& a = overlay.args;
        if (a[3] == 192 + 40 && a[5] == 20) {
            assert_middle_strip_rgba(overlay);
            ++middle_slices;
        } else if (a[5] == 103) {
            assert((a[2] == 578 || a[2] == 790) && a[3] == 192 && a[4] == 50);
        } else {
            assert(a[2] == 628 && a[4] > 0 && a[4] <= 160);
            assert((a[3] == 192 && a[5] == 40) || (a[3] == 252 && a[5] == 43));
        }
    }
    assert(middle_slices > 0);
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
    test_pressed_rgba_fixture();
    setup();
    assert(ui_draw_native_text_button_background({0, 0, 470, 112}, 0));
    assert(g_draws.size() == 1 && g_loads == 1);
    assert((g_draws[0].args == std::array<int32_t, 12>{0, 0, 2, 124, 470, 112, 0, 0,
                                                        1, 0, 100, 0}));
    assert(g_draws[0].group == &g_group);

    setup();
    assert(ui_draw_native_text_button_background({10, 20, 940, 224}, 1));
    assert(g_draws.size() > 1 && g_draws[0].args[10] == 200);
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
    // UI_006.png.jpg is RGBA. The crop includes the capsule contour and neighboring
    // sampling guards; transparent RGB values alone do not identify visible pixels.
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

    // 300x92 is the MoreGames rect. The cap+70px-center model has no even-percent fit within
    // the 1 px tolerance (best even percent 82 is off by 1.2), so the renderer must fall back
    // to a contiguous narrower center crop while keeping the original 100 px rounded caps,
    // the requested rect size/style/position, and half-open seams.
    for (uint8_t pressed = 0; pressed <= 1; ++pressed) {
        setup();
        const UiRect more_games_rect{0, 0, 300, 92};
        assert(ui_draw_native_text_button_background(more_games_rect, pressed));
        assert(pressed ? g_draws.size() > 3 : g_draws.size() == 3);
        const std::vector<DrawCall> overlays(g_draws.begin() + 3, g_draws.end());
        g_draws.resize(3);
        const int32_t percent = g_draws[0].args[10];
        assert(percent >= 1 && percent <= 400);
        assert(g_draws[1].args[10] == percent && g_draws[2].args[10] == percent);
        // Caps keep their full 100 px source; center is a contiguous crop at the cap edge.
        assert(g_draws[0].args[2] == 35 && g_draws[0].args[4] == 100);
        assert(g_draws[1].args[2] == 135 && g_draws[1].args[4] >= 1 && g_draws[1].args[4] <= 70);
        assert(g_draws[2].args[2] == 340 && g_draws[2].args[4] == 100);
        for (size_t i = 0; i < g_draws.size(); ++i) {
            assert(g_draws[i].args[3] == 146 && g_draws[i].args[5] == 77);
            assert(g_draws[i].args[8] == 1 && g_draws[i].args[9] == 0 && g_draws[i].args[11] == 0);
        }
        // Half-open destination seams: each slice starts exactly where the previous ends.
        assert(g_draws[0].args[0] + g_draws[0].args[4] * percent / 100 == g_draws[1].args[0]);
        assert(g_draws[1].args[0] + g_draws[1].args[4] * percent / 100 == g_draws[2].args[0]);
        const int32_t drawn_width =
            g_draws[2].args[0] + g_draws[2].args[4] * percent / 100 - g_draws[0].args[0];
        // Drawn width is exactly the sum of the floored slice widths: no width+1 extent.
        assert(drawn_width == g_draws[0].args[4] * percent / 100 +
            g_draws[1].args[4] * percent / 100 + g_draws[2].args[4] * percent / 100);
        // Everything stays inside the requested rect.
        assert(g_draws[0].args[0] >= more_games_rect.x);
        assert(g_draws[0].args[0] + drawn_width <= more_games_rect.x + more_games_rect.w);
        for (size_t i = 0; i < g_draws.size(); ++i) {
            const int32_t dest_right = g_draws[i].args[0] + g_draws[i].args[4] * percent / 100;
            const int32_t dest_bottom = g_draws[i].args[1] + g_draws[i].args[5] * percent / 100;
            assert(dest_right <= more_games_rect.x + more_games_rect.w);
            assert(dest_bottom <= more_games_rect.y + more_games_rect.h);
        }
        // The capsule stays centered in the requested rect within a couple logical px.
        const int32_t left_margin = g_draws[0].args[0] - more_games_rect.x;
        const int32_t right_margin =
            more_games_rect.x + more_games_rect.w - (g_draws[0].args[0] + drawn_width);
        assert(std::abs(left_margin - right_margin) <= 2);
        // Label at font 1 must still fit and center on the drawn capsule.
        assert(ui_draw_native_text_button_label(more_games_rect, "打开", 0, 1));
        assert(g_text_font == 1 && g_font_height == 26);
        const int32_t capsule_center_x = g_draws[0].args[0] + drawn_width / 2;
        assert(std::abs(g_text_x - capsule_center_x) <= 2);
        const int32_t image_height = g_draws[0].args[5] * percent / 100;
        assert(g_draws[0].args[1] + image_height <= more_games_rect.y + more_games_rect.h);
        size_t middle_band_slices = 0;
        for (const auto& overlay : overlays) {
            const auto& a = overlay.args;
            assert(overlay.group == &g_toggle_group);
            // Halo is allowed outside the hit rect; opaque contour, not the
            // full low-alpha source canvas, must match the normal capsule.
            if (a[3] == 192 + 40 && a[5] == 20) {
                assert_middle_strip_rgba(overlay);
                ++middle_band_slices;
            }
            assert(a[6] == 0 && a[7] == 0 && a[8] == 1 && a[9] == 0);
        }
        assert(pressed ? middle_band_slices > 0 : middle_band_slices == 0);
        if (pressed) assert_visible_press_contour(g_draws, overlays);
    }

    // Degenerate tiny rects stay rejected instead of drawing a sliver.
    struct RenderCase { UiRect rect; int32_t pressed_scale; };
    const RenderCase render_cases[] = {
        {full_button, 52}, {settings_toggle_rect, 86}, {{0, 13, 300, 65}, 71},
        {{-100, -80, 220, 48}, 52}, {{53, 71, 440, 160}, 172}, {{0, 0, 1880, 448}, 492}};
    for (const auto& render_case : render_cases) {
        const UiRect rect = render_case.rect;
        setup();
        assert(ui_draw_native_text_button_background(rect, 0));
        const auto normal = g_draws;
        g_draws.clear();
        assert(ui_draw_native_text_button_background(rect, 1));
        assert(g_draws.size() > normal.size());
        assert(g_draws[normal.size()].args[10] == render_case.pressed_scale);
        for (size_t i = 0; i < normal.size(); ++i) {
            assert(g_draws[i].args == normal[i].args && g_draws[i].group == normal[i].group);
        }
        assert_visible_press_contour(normal, {g_draws.begin() + normal.size(), g_draws.end()});
        assert(g_clip_calls == 0 && g_reset_clip_calls == 0);
        assert(g_draws.back().group == &g_toggle_group);
        g_draws.clear();
        // Pressed=false is also the engine move-out/release draw input. Both
        // existing appearances must restore the identical normal background.
        assert(ui_draw_native_text_button_background(rect, 0));
        assert(g_draws.size() == normal.size());
        for (size_t i = 0; i < normal.size(); ++i) assert(g_draws[i].args == normal[i].args);
        setup();
        g_press_available = false;
        assert(!ui_draw_native_text_button_background(rect, 1) && g_draws.empty());
        assert(ui_draw_native_text_button_background(rect, 0));
    }
    setup();
    assert(ui_draw_native_text_button_background({0, 13, 300, 65}, 0));
    // Decorated compact aspect keeps the entire original loc, not the capsule crop.
    assert(g_draws.size() == 1 && g_draws[0].args[2] == 2 && g_draws[0].args[4] == 470);

    setup();
    assert(!ui_draw_native_text_button_background({0, 0, 2, 2}, 0));
    assert(!ui_draw_native_text_button_background({0, 0, 4, 3}, 0));
    assert(g_draws.empty());

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
    // Explicit appearance is independent of aspect ratio; legacy cases above
    // continue to verify the original ratio-driven geometry and RGBA strip.
    for (const uint32_t appearance : {qol::contracts::kUiTextButtonCapsule,
                                       qol::contracts::kUiTextButtonDecorated}) {
        setup();
        const qol::contracts::UiTextButtonPaintDesc desc{{0, 0, 220, 80}, appearance, 0, 0};
        qol::contracts::UiTextButtonPaintLayout layout{};
        assert(qol::contracts::succeeded(qol::game::ui_layout_native_text_button(desc, &layout)));
        assert(qol::contracts::succeeded(qol::game::ui_draw_native_text_button(desc, "打开", 0, 0)));
        assert(g_drawn_label == "打开" && g_text_font == 0);
        const auto normal = g_draws;
        g_draws.clear();
        assert(qol::contracts::succeeded(qol::game::ui_draw_native_text_button(desc, "打开", 0, 1)));
        assert(g_draws.size() > normal.size());
        for (size_t i = 0; i < normal.size(); ++i) assert(normal[i].args == g_draws[i].args);
        for (size_t i = normal.size(); i < g_draws.size(); ++i) {
            if (g_draws[i].args[5] == 20) assert_middle_strip_rgba(g_draws[i]);
        }
        if (appearance == qol::contracts::kUiTextButtonCapsule) {
            assert(normal.size() == 3 && normal.front().args[2] == 35);
            assert(layout.normal_bounds.w == 189);
        } else {
            assert(normal.front().args[2] == 2 && normal.front().args[4] == 33);
            assert(layout.normal_bounds.h == 80);
        }
    }
    setup();
    qol::contracts::UiTextButtonPaintDesc menu{{0, 0, 300, 65},
        qol::contracts::kUiTextButtonDecorated, 0, 0};
    qol::contracts::UiTextButtonPaintLayout old_menu{}, enlarged{};
    assert(qol::contracts::succeeded(qol::game::ui_layout_native_text_button(menu, &old_menu)));
    assert(old_menu.normal_bounds.w == 272 && old_menu.normal_bounds.h == 64);
    menu.visual_rect = old_menu.normal_bounds;
    menu.visual_rect.y -= 16;
    menu.visual_rect.h = 96;
    menu.font_size = 28;
    assert(qol::contracts::succeeded(qol::game::ui_layout_native_text_button(menu, &enlarged)));
    assert(enlarged.normal_bounds.w == 272 && enlarged.normal_bounds.h == 96);
    assert(2 * old_menu.normal_bounds.y + 64 == 2 * enlarged.normal_bounds.y + 96);
    assert(qol::contracts::succeeded(qol::game::ui_draw_native_text_button(menu, "模块设置", 0, 1)));
    assert(g_text_font == 3 && g_font_height == 30 && g_drawn_label == "模块设置");
    g_draws.clear();
    menu.font_size = 30;
    assert(qol::game::ui_draw_native_text_button(menu, "模块设置", 0, 1).code == kQolErrUnsupported);
    assert(g_draws.empty());
    return 0;
}
