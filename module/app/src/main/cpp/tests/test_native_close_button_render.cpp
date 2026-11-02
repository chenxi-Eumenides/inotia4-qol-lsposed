#include <array>
#include <cassert>
#include <cstdint>
#include <cstring>
#include <vector>

#include "game/access/game_access.h"
#include "game/access/ui_native_close_button.h"

namespace {

struct DrawCall {
    std::array<int32_t, 12> args;
    void* group;
};

alignas(8) std::array<uint8_t, 0x18> g_glyph_normal{};
alignas(8) std::array<uint8_t, 0x18> g_glyph_pressed{};
alignas(8) std::array<uint8_t, 0x18> g_frame_location{};
uint8_t g_glyph_group_byte{};
uint8_t g_frame_group_byte{};
std::vector<DrawCall> g_draws;
int g_glyph_loads = 0;
int g_frame_loads = 0;
bool g_glyph_group_available = true;
bool g_frame_group_available = true;
bool g_normal_available = true;
bool g_pressed_available = true;
bool g_frame_available = true;
bool g_main_thread = true;

void* fake_group(int32_t unit) {
    if (unit == 0x0f) return g_glyph_group_available ? &g_glyph_group_byte : nullptr;
    if (unit == 0x59) return g_frame_group_available ? &g_frame_group_byte : nullptr;
    assert(false);
    return nullptr;
}

void* fake_loc(int32_t unit, int32_t loc) {
    if (unit == 0x0f) {
        if (loc == 0x3a) return g_normal_available ? g_glyph_normal.data() : nullptr;
        if (loc == 0x34) return g_pressed_available ? g_glyph_pressed.data() : nullptr;
    } else if (unit == 0x59 && loc == 0x3) {
        return g_frame_available ? g_frame_location.data() : nullptr;
    }
    assert(false);
    return nullptr;
}

void fake_load(int32_t unit) {
    if (unit == 0x0f) ++g_glyph_loads;
    else if (unit == 0x59) ++g_frame_loads;
    else assert(false);
}

void fake_draw(void* group, int32_t x, int32_t y, int32_t sx, int32_t sy, int32_t sw,
               int32_t sh, int32_t ox, int32_t oy, int32_t mode, int32_t alpha,
               int32_t percent, int32_t rotation) {
    g_draws.push_back({{x, y, sx, sy, sw, sh, ox, oy, mode, alpha, percent, rotation}, group});
}

void set_field(std::array<uint8_t, 0x18>& location, size_t offset, int32_t value) {
    std::memcpy(location.data() + offset, &value, sizeof(value));
}

void set_layer(std::array<uint8_t, 0x18>& location, int32_t sx, int32_t sy, int32_t w, int32_t h) {
    set_field(location, IMAGE_LOC_SOURCE_X, sx);
    set_field(location, IMAGE_LOC_SOURCE_Y, sy);
    set_field(location, IMAGE_LOC_SOURCE_WIDTH, w);
    set_field(location, IMAGE_LOC_SOURCE_HEIGHT, h);
    set_field(location, IMAGE_LOC_OFFSET_X, 0);
    set_field(location, IMAGE_LOC_OFFSET_Y, 0);
}

void setup() {
    g_draws.clear();
    g_glyph_loads = 0;
    g_frame_loads = 0;
    g_glyph_group_available = true;
    g_frame_group_available = true;
    g_normal_available = true;
    g_pressed_available = true;
    g_frame_available = true;
    g_main_thread = true;
    // Explorer-confirmed shared atlas rects:
    //   X glyph unit 0x0f loc 0x3a (234,441,38,39); pressed plate loc 0x34 (275,419,62,61).
    //   Back/close frame unit 0x59 loc 0x3 (392,72,78,86).
    set_layer(g_glyph_normal, 234, 441, 38, 39);
    set_layer(g_glyph_pressed, 275, 419, 62, 61);
    set_layer(g_frame_location, 392, 72, 78, 86);
}

// loc3 金属框分片（相对 loc3 原点）：上/下/右边 + 右角弧；loc3 左侧开口、中央为返回箭头，
// 故不取左中与中央，避免把箭头画进 X 按钮。条带与角弧刻意重叠以免接缝。
struct FrameSlice {
    int32_t x, y, w, h;
};
constexpr FrameSlice kSlices[5] = {
    {0, 4, 78, 5}, {0, 76, 78, 5}, {68, 4, 6, 78}, {60, 4, 18, 18}, {60, 64, 18, 22},
};

}  // namespace

decltype(fn_imgsys_unit_load) fn_imgsys_unit_load = &fake_load;
decltype(fn_imgsys_get_group) fn_imgsys_get_group = &fake_group;
decltype(fn_imgsys_get_loc) fn_imgsys_get_loc = &fake_loc;
decltype(fn_sgl_draw_texture_part_ex) fn_sgl_draw_texture_part_ex = &fake_draw;

bool game_memory_accessible(const void* address, size_t, char) { return address != nullptr; }

namespace qol::game {
bool game_host_is_main_thread() { return g_main_thread; }
}

int main() {
    using qol::game::ui_draw_native_close_button;

    // 原生尺寸盒：X 与返回共用 loc3 金属框，X 字形 1:1（38×39 @ +(15,24)）。
    setup();
    assert(ui_draw_native_close_button({0, 0, 78, 86}, 0));
    assert(g_glyph_loads >= 1 && g_frame_loads >= 1);
    assert(g_draws.size() == 6);  // 5 条金属框分片 + 1 个 X 字形
    for (size_t i = 0; i < 5; ++i) {
        const FrameSlice& slice = kSlices[i];
        assert((g_draws[i].args == std::array<int32_t, 12>{
            slice.x, slice.y, 392 + slice.x, 72 + slice.y, slice.w, slice.h, 0, 0, 1, 0, 100, 0}));
        assert(g_draws[i].group == &g_frame_group_byte);
    }
    // 字形：src(234,441,38,39) @ +(15,24)，1:1 不缩放，来自 0x0f 图组。
    assert((g_draws[5].args == std::array<int32_t, 12>{
        15, 24, 234, 441, 38, 39, 0, 0, 1, 0, 100, 0}));
    assert(g_draws[5].group == &g_glyph_group_byte);

    // 按下：叠加 loc0x34 橙色底板 1:1 @ +(3,12)，画在字形之后。
    setup();
    assert(ui_draw_native_close_button({0, 0, 78, 86}, 1));
    assert(g_draws.size() == 7);
    assert((g_draws[6].args == std::array<int32_t, 12>{
        3, 12, 275, 419, 62, 61, 0, 0, 1, 0, 100, 0}));
    assert(g_draws[6].group == &g_glyph_group_byte);

    // 盒尺寸不等于 78×86 时，原生框/字形居中于 rect（半格向下取整）。
    setup();
    assert(ui_draw_native_close_button({100, 200, 90, 90}, 0));
    assert(g_draws.size() == 6);
    // origin = (100+6, 200+2) = (106,202)
    assert((g_draws[0].args == std::array<int32_t, 12>{106, 206, 392, 76, 78, 5, 0, 0, 1, 0, 100, 0}));
    assert((g_draws[5].args == std::array<int32_t, 12>{121, 226, 234, 441, 38, 39, 0, 0, 1, 0, 100, 0}));

    // 0x59 金属框不可用时降级为仅字形，不静默改尺寸。
    setup();
    g_frame_group_available = false;
    assert(ui_draw_native_close_button({0, 0, 78, 86}, 0));
    assert(g_draws.size() == 1 && g_draws[0].group == &g_glyph_group_byte);
    setup();
    g_frame_available = false;
    assert(ui_draw_native_close_button({0, 0, 78, 86}, 0) && g_draws.size() == 1);
    setup();
    g_frame_loads = 0;
    assert(ui_draw_native_close_button({0, 0, 78, 86}, 0) && g_frame_loads >= 1);

    // 参数与资源不可用：不绘制。
    setup();
    assert(!ui_draw_native_close_button({0, 0, 78, 86}, 2) && g_draws.empty());
    setup();
    assert(!ui_draw_native_close_button({0, 0, 0, 86}, 0) && g_draws.empty());
    setup();
    g_main_thread = false;
    assert(!ui_draw_native_close_button({0, 0, 78, 86}, 0) && g_draws.empty());
    setup();
    g_glyph_group_available = false;
    assert(!ui_draw_native_close_button({0, 0, 78, 86}, 0) && g_draws.empty());
    setup();
    g_normal_available = false;
    assert(!ui_draw_native_close_button({0, 0, 78, 86}, 0) && g_draws.empty());
    setup();
    g_pressed_available = false;
    assert(!ui_draw_native_close_button({0, 0, 78, 86}, 1) && g_draws.empty());
    // 按下底板缺失不得破坏常态路径。
    assert(ui_draw_native_close_button({0, 0, 78, 86}, 0) && g_draws.size() == 6);
    return 0;
}
