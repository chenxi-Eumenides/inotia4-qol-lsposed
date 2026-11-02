#include <array>
#include <cassert>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <vector>

#include "game/access/game_access.h"
#include "game/access/ui_native_toggle.h"

namespace {

struct DrawCall {
    std::array<int32_t, 12> args;
    void* group;
};

alignas(8) std::array<std::array<uint8_t, 0x18>, 17> g_locs{};
uint8_t g_group{};
std::vector<DrawCall> g_draws;
bool g_main_thread = true;
bool g_group_available = true;
bool g_missing_loc = false;
int32_t g_unit = 0x4f;
int g_loads = 0;

void* fake_group(int32_t unit) {
    assert(unit == g_unit);
    return g_group_available ? &g_group : nullptr;
}

void* fake_loc(int32_t unit, int32_t loc) {
    assert(unit == g_unit);
    if (g_missing_loc && loc == 0x9) return nullptr;
    return g_locs.at(static_cast<size_t>(loc)).data();
}

void fake_load(int32_t unit) {
    assert(unit == g_unit);
    ++g_loads;
}

int32_t fake_group_type() { return g_unit; }

void fake_draw(void* group, int32_t x, int32_t y, int32_t sx, int32_t sy, int32_t sw,
               int32_t sh, int32_t ox, int32_t oy, int32_t mode, int32_t alpha,
               int32_t percent, int32_t rotation) {
    g_draws.push_back({{x, y, sx, sy, sw, sh, ox, oy, mode, alpha, percent, rotation}, group});
}

void set_field(int32_t loc, size_t offset, int32_t value) {
    std::memcpy(g_locs.at(static_cast<size_t>(loc)).data() + offset, &value, sizeof(value));
}

void set_loc(int32_t loc, int32_t sx, int32_t sy, int32_t width, int32_t height,
             int32_t ox, int32_t oy) {
    set_field(loc, IMAGE_LOC_SOURCE_X, sx);
    set_field(loc, IMAGE_LOC_SOURCE_Y, sy);
    set_field(loc, IMAGE_LOC_SOURCE_WIDTH, width);
    set_field(loc, IMAGE_LOC_SOURCE_HEIGHT, height);
    set_field(loc, IMAGE_LOC_OFFSET_X, ox);
    set_field(loc, IMAGE_LOC_OFFSET_Y, oy);
}

void setup() {
    g_draws.clear();
    g_loads = 0;
    g_main_thread = true;
    g_group_available = true;
    g_missing_loc = false;
    set_loc(0x0f, 31, 32, 100, 40, 3, -2);
    set_loc(0x10, 41, 42, 100, 40, 5, -4);
    set_loc(0x9, 51, 52, 100, 40, 7, -6);
    set_loc(0x7, 61, 62, 100, 40, 9, -8);
}

}  // namespace

decltype(fn_get_group_title_img_type) fn_get_group_title_img_type = &fake_group_type;
decltype(fn_imgsys_unit_load) fn_imgsys_unit_load = &fake_load;
decltype(fn_imgsys_get_group) fn_imgsys_get_group = &fake_group;
decltype(fn_imgsys_get_loc) fn_imgsys_get_loc = &fake_loc;
decltype(fn_sgl_draw_texture_part_ex) fn_sgl_draw_texture_part_ex = &fake_draw;

bool game_memory_accessible(const void* address, size_t, char) { return address != nullptr; }

namespace qol::game {
bool game_host_is_main_thread() { return g_main_thread; }
}

int main() {
    using qol::game::ui_draw_native_toggle;
    setup();
    assert(ui_draw_native_toggle(UiRect{10, 20, 200, 80}, 1, 0));
    assert(g_draws.size() == 1);
    assert((g_draws[0].args == std::array<int32_t, 12>{10, 20, 31, 32, 100, 40, 3, -2,
                                                       1, 0, 200, 0}));
    assert(g_draws[0].group == &g_group && g_loads == 1);

    setup();
    assert(ui_draw_native_toggle(UiRect{0, 0, 200, 80}, 0, 1));
    assert(g_draws.size() == 3);
    assert(g_draws[0].args[2] == 41 && g_draws[0].args[8] == 1);
    assert(g_draws[1].args[2] == 51 && g_draws[2].args[2] == 61);
    for (const auto& draw : g_draws) {
        assert(draw.args[0] == 0 && draw.args[1] == 0);
        assert(draw.args[6] == 5 && draw.args[7] == -4);
        assert(draw.args[10] == 200 && draw.args[8] == 1 && draw.args[9] == 0 && draw.args[11] == 0);
    }
    // Real title atlas metadata: pressed canvas adds an 8 px halo per side.
    for (const UiRect rect : {UiRect{10, 20, 220, 80}, UiRect{30, 40, 492, 174}}) {
        setup();
        set_loc(0x0f, 2, 491, 246, 87, 0, 0);
        set_loc(0x7, 578, 192, 262, 103, 0, 0);
        assert(ui_draw_native_toggle(rect, 1, 1));
        assert(g_draws.size() == 2);
        const auto& base = g_draws[0].args;
        const auto& press = g_draws[1].args;
        const int32_t percent = base[10];
        assert(press[0] == base[0] - 8 * percent / 100);
        assert(press[1] == base[1] - 8 * percent / 100);
        assert(std::abs(2 * press[0] + press[4] * percent / 100 -
                        (2 * base[0] + base[4] * percent / 100)) <= 1);
        assert(std::abs(2 * press[1] + press[5] * percent / 100 -
                         (2 * base[1] + base[5] * percent / 100)) <= 1);
    }
    for (int32_t value = 0; value <= 1; ++value) {
        setup();
        set_loc(0x0f, 2, 491, 246, 87, 0, 0);
        set_loc(0x10, 251, 491, 246, 87, 0, 0);
        set_loc(0x9, 578, 298, 246, 87, 0, 0);
        set_loc(0x7, 578, 192, 262, 103, 0, 0);
        const UiRect rect{-100, -80, 220, 80};
        assert(ui_draw_native_toggle(rect, value, 0));
        const auto normal = g_draws;
        g_draws.clear();
        assert(ui_draw_native_toggle(rect, value, 1));
        assert(g_draws.size() == normal.size() + 1);
        for (size_t i = 0; i < normal.size(); ++i) assert(g_draws[i].args == normal[i].args);
        const auto& base = normal.front().args;
        const auto& press = g_draws.back().args;
        assert(base[10] == 89 && press[10] == 89);
        // Independently measured source alpha bounds, transformed at 89%.
        // A 262x103 canvas refit into rect would fail these visible-edge checks.
        const int32_t normal_left = base[0] + 13 * 89 / 100;
        const int32_t normal_right = base[0] + 233 * 89 / 100;
        const int32_t normal_top = base[1] + 13 * 89 / 100;
        const int32_t normal_bottom = base[1] + 74 * 89 / 100;
        const int32_t pressed_left = press[0] + 21 * 89 / 100;
        const int32_t pressed_right = press[0] + 241 * 89 / 100;
        const int32_t pressed_top = press[1] + 21 * 89 / 100;
        const int32_t pressed_bottom = press[1] + 82 * 89 / 100;
        assert(normal_right - normal_left == 196 && normal_bottom - normal_top == 54);
        assert(pressed_left == normal_left && pressed_right == normal_right);
        assert(pressed_top == normal_top && pressed_bottom == normal_bottom);
        // Release/move-out uses the same normal geometry, without a stale overlay.
        g_draws.clear();
        assert(ui_draw_native_toggle(rect, value, 0));
        assert(g_draws.size() == normal.size());
        for (size_t i = 0; i < normal.size(); ++i) assert(g_draws[i].args == normal[i].args);
    }

    setup();
    assert(ui_draw_native_toggle(UiRect{10, 20, 100, 40}, 1, 0));
    assert(g_draws[0].args[0] == 10 && g_draws[0].args[1] == 20 && g_draws[0].args[10] == 100);
    setup();
    assert(ui_draw_native_toggle(UiRect{0, 0, 1, 1}, 1, 0));
    assert(g_draws[0].args[10] == 1);
    setup();
    assert(ui_draw_native_toggle(UiRect{0, 0, 400, 160}, 1, 0));
    assert(g_draws[0].args[10] == 400);

    setup();
    assert(!ui_draw_native_toggle(UiRect{0, 0, 0, 40}, 1, 0) && g_draws.empty());
    assert(!ui_draw_native_toggle(UiRect{std::numeric_limits<int32_t>::max(), 0, 1, 1}, 1, 0));
    assert(!ui_draw_native_toggle(UiRect{0, 0, 999, 400}, 1, 0));
    assert(!ui_draw_native_toggle(UiRect{0, 0, 200, 80}, 2, 0));
    assert(!ui_draw_native_toggle(UiRect{0, 0, 200, 80}, 1, 2));
    assert(g_draws.empty());

    setup();
    g_main_thread = false;
    assert(!ui_draw_native_toggle(UiRect{0, 0, 200, 80}, 1, 0) && g_draws.empty());
    setup();
    g_group_available = false;
    assert(!ui_draw_native_toggle(UiRect{0, 0, 200, 80}, 1, 0) && g_draws.empty());
    setup();
    g_missing_loc = true;
    assert(!ui_draw_native_toggle(UiRect{0, 0, 200, 80}, 0, 0) && g_draws.empty());
    setup();
    fn_sgl_draw_texture_part_ex = nullptr;
    assert(!ui_draw_native_toggle(UiRect{0, 0, 200, 80}, 1, 0) && g_draws.empty());
    fn_sgl_draw_texture_part_ex = &fake_draw;
    setup();
    fn_get_group_title_img_type = nullptr;
    assert(!ui_draw_native_toggle(UiRect{0, 0, 200, 80}, 1, 0) && g_draws.empty());
    fn_get_group_title_img_type = &fake_group_type;
    setup();
    fn_imgsys_unit_load = nullptr;
    assert(!ui_draw_native_toggle(UiRect{0, 0, 200, 80}, 1, 0) && g_draws.empty());
    fn_imgsys_unit_load = &fake_load;
    setup();
    fn_imgsys_get_group = nullptr;
    assert(!ui_draw_native_toggle(UiRect{0, 0, 200, 80}, 1, 0) && g_draws.empty());
    fn_imgsys_get_group = &fake_group;
    setup();
    fn_imgsys_get_loc = nullptr;
    assert(!ui_draw_native_toggle(UiRect{0, 0, 200, 80}, 1, 0) && g_draws.empty());
    return 0;
}
