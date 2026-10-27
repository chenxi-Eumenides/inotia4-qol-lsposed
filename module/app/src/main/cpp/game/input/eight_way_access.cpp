#include "game/input/eight_way_access.h"

#include <cstddef>
#include <cstdint>

#include "game/access/game_access.h"
#include "game/symbols/game_symbols.h"

namespace qol::game::input::access {
namespace {

using ScreenHeightFn = int (*)();
using TouchOffsetFn = void* (*)();
using MapBlockingFn = int32_t (*)(int32_t, int32_t);

struct AbsPoint {
    int64_t x = 0;
    int64_t y = 0;
};

using GetAbsolutePointFn = AbsPoint (*)(void*);
using VirtualKeyRootFn = void* (*)();
using FindControlByIdFn = void* (*)(void*, int32_t);
using SetFocusFn = void (*)(int32_t, int32_t);

struct TouchOffset {
    int64_t x = 0;
    int64_t y = 0;
};

bool readable(const void* address, size_t size) {
    return address != nullptr && game_memory_accessible(address, size, 'r');
}

bool writable(const void* address, size_t size) {
    return address != nullptr && game_memory_accessible(address, size, 'w');
}

int32_t* key_code_ptr() {
    if (g_base == 0) return nullptr;
    auto* ptr = reinterpret_cast<int32_t*>(g_base + G_KEY_CODE_VMA);
    return writable(ptr, sizeof(int32_t)) ? ptr : nullptr;
}

ScreenHeightFn screen_height_fn() {
    static ScreenHeightFn fn = nullptr;
    if (fn == nullptr && g_base != 0) {
        fn = reinterpret_cast<ScreenHeightFn>(
            g_base + fn_resolve("F_SGL_GET_SCREEN_HEIGHT_VMA", F_SGL_GET_SCREEN_HEIGHT_VMA));
    }
    return fn;
}

TouchOffsetFn touch_offset_fn() {
    static TouchOffsetFn fn = nullptr;
    if (fn == nullptr && g_base != 0) {
        fn = reinterpret_cast<TouchOffsetFn>(
            g_base + fn_resolve("F_TOUCH_HANDLE_GET_CONTROL_OFFSET_VMA",
                                F_TOUCH_HANDLE_GET_CONTROL_OFFSET_VMA));
    }
    return fn;
}

GetAbsolutePointFn absolute_point_fn() {
    static GetAbsolutePointFn fn = nullptr;
    if (fn == nullptr && g_base != 0) {
        fn = reinterpret_cast<GetAbsolutePointFn>(
            g_base + fn_resolve("F_CONTROL_OBJECT_GET_ABSOLUTE_POINT_VMA",
                                F_CONTROL_OBJECT_GET_ABSOLUTE_POINT_VMA));
    }
    return fn;
}

VirtualKeyRootFn virtual_key_root_fn() {
    static VirtualKeyRootFn fn = nullptr;
    if (fn == nullptr && g_base != 0) {
        fn = reinterpret_cast<VirtualKeyRootFn>(
            g_base + fn_resolve("F_INPUT_LAYER_GET_VIRTUAL_KEY_VMA",
                                F_INPUT_LAYER_GET_VIRTUAL_KEY_VMA));
    }
    return fn;
}

FindControlByIdFn find_control_by_id_fn() {
    static FindControlByIdFn fn = nullptr;
    if (fn == nullptr && g_base != 0) {
        fn = reinterpret_cast<FindControlByIdFn>(
            g_base + fn_resolve("F_CONTROL_OBJECT_FIND_BY_ID_VMA",
                                F_CONTROL_OBJECT_FIND_BY_ID_VMA));
    }
    return fn;
}


MapBlockingFn map_blocking_tile_fn() {
    static MapBlockingFn fn = nullptr;
    if (fn == nullptr && g_base != 0) {
        fn = reinterpret_cast<MapBlockingFn>(
            g_base + fn_resolve("F_MAP_IS_BLOCKING_VMA", F_MAP_IS_BLOCKING_VMA));
    }
    return fn;
}

SetFocusFn set_focus_fn() {
    static SetFocusFn fn = nullptr;
    if (fn == nullptr && g_base != 0) {
        fn = reinterpret_cast<SetFocusFn>(
            g_base + fn_resolve("F_MAP_SET_FOCUS_VMA", F_MAP_SET_FOCUS_VMA));
    }
    return fn;
}

bool read_got_i32(uintptr_t got_vma, int32_t* out) {
    if (out == nullptr || g_base == 0) return false;
    const auto* slot = reinterpret_cast<const uintptr_t*>(g_base + got_vma);
    if (!readable(slot, sizeof(uintptr_t))) return false;
    const auto* value = reinterpret_cast<const int32_t*>(*slot);
    if (!readable(value, sizeof(int32_t))) return false;
    *out = *value;
    return true;
}

}  // namespace

bool key_code(int32_t* out) {
    if (out == nullptr) return false;
    const int32_t* ptr = key_code_ptr();
    if (ptr == nullptr) return false;
    *out = *ptr;
    return true;
}

bool write_key_code(int32_t code) {
    int32_t* ptr = key_code_ptr();
    if (ptr == nullptr) return false;
    *ptr = code;
    return true;
}

bool read_key_table(int32_t out[4]) {
    if (out == nullptr || g_base == 0) return false;
    const auto* table = reinterpret_cast<const int32_t*>(g_base + G_KEYSET_DATA_VMA);
    if (!readable(table, sizeof(int32_t) * 4)) return false;
    for (int i = 0; i < 4; ++i) out[i] = table[i];
    return true;
}

bool press_active() {
    if (g_base == 0) return false;
    const auto* ptr = reinterpret_cast<const uint8_t*>(g_base + G_KEY_BPRESSED_VMA);
    return readable(ptr, sizeof(uint8_t)) && *ptr != 0;
}

bool write_press_active(bool active) {
    if (g_base == 0) return false;
    auto* ptr = reinterpret_cast<uint8_t*>(g_base + G_KEY_BPRESSED_VMA);
    if (!writable(ptr, sizeof(uint8_t))) return false;
    *ptr = active ? 1 : 0;
    return true;
}

int32_t screen_height() {
    const ScreenHeightFn fn = screen_height_fn();
    return fn != nullptr ? fn() : 0;
}

bool touch_offset(int64_t* x, int64_t* y) {
    if (x == nullptr || y == nullptr) return false;
    const TouchOffsetFn fn = touch_offset_fn();
    if (fn == nullptr) return false;
    const void* raw = fn();
    if (!readable(raw, sizeof(TouchOffset))) return false;
    const auto* offset = static_cast<const TouchOffset*>(raw);
    *x = offset->x;
    *y = offset->y;
    return true;
}

bool control_absolute_point(void* ctrl, int64_t* x, int64_t* y) {
    if (ctrl == nullptr || x == nullptr || y == nullptr) return false;
    const GetAbsolutePointFn fn = absolute_point_fn();
    if (fn == nullptr) return false;
    const AbsPoint point = fn(ctrl);
    *x = point.x;
    *y = point.y;
    return true;
}

bool read_position(const void* ch, int16_t* x, int16_t* y) {
    if (ch == nullptr || x == nullptr || y == nullptr) return false;
    const auto* bytes = static_cast<const uint8_t*>(ch);
    if (!readable(bytes + C_POS_X, sizeof(int16_t)) ||
        !readable(bytes + C_POS_Y, sizeof(int16_t))) {
        return false;
    }
    *x = *reinterpret_cast<const int16_t*>(bytes + C_POS_X);
    *y = *reinterpret_cast<const int16_t*>(bytes + C_POS_Y);
    return true;
}

bool write_position(void* ch, int16_t x, int16_t y) {
    if (ch == nullptr) return false;
    auto* bytes = static_cast<uint8_t*>(ch);
    if (!writable(bytes + C_POS_X, sizeof(int16_t)) ||
        !writable(bytes + C_POS_Y, sizeof(int16_t))) {
        return false;
    }
    *reinterpret_cast<int16_t*>(bytes + C_POS_X) = x;
    *reinterpret_cast<int16_t*>(bytes + C_POS_Y) = y;
    return true;
}

bool has_path(const void* ch) {
    if (ch == nullptr) return false;
    const auto* bytes = static_cast<const uint8_t*>(ch);
    if (!readable(bytes + C_PATH_LIST, sizeof(void*))) return true;
    return *reinterpret_cast<void* const*>(bytes + C_PATH_LIST) != nullptr;
}

bool prepare_touch_move(void* ch, int32_t dir) {
    if (ch == nullptr || dir < 0 || dir > 3 || fn_char_set_direction == nullptr) return false;
    // 预置朝向使原版转向闸门走“朝向相同”分支；随后原版仍会调用 CHAR_SetDirection
    // 设置子朝向。使用原函数而不是直接写 [ch+6]，保留原版对特殊状态的方向锁定判断。
    fn_char_set_direction(ch, dir);
    return clear_wait_coord(ch);
}

bool clear_wait_coord(void* ch) {
    if (ch == nullptr) return false;
    auto* wait_coord = static_cast<uint8_t*>(ch) + C_WAIT_COORD;
    if (!writable(wait_coord, sizeof(uint8_t))) return false;
    *wait_coord = 0;
    return true;
}

int32_t tile_blocking(int32_t tile_x, int32_t tile_y) {
    const MapBlockingFn fn = map_blocking_tile_fn();
    return fn != nullptr ? fn(tile_x, tile_y) : 1;
}

int32_t map_blocking_query(int32_t x, int32_t y, MapBlockingQueryFn original) {
    if (original == nullptr) return 1;

    int32_t width = 0;
    int32_t height = 0;
    if (!read_got_i32(G_MAP_WIDTH_GOT_VMA, &width) ||
        !read_got_i32(G_MAP_HEIGHT_GOT_VMA, &height) || width <= 0 || height <= 0) {
        return original(x, y);
    }

    // 边界按实际坐标判断，避免把贴边角色规整到地图内。
    if (x <= 7 || y <= 7 || x >= width * 16 - 7 || y >= height * 16 - 7) return 1;

    // 8px 对齐时原函数采样完整，直接透传。
    if ((x & 7) == 0 && (y & 7) == 0) return original(x, y);

    // 非对齐位置：实际 16px 碰撞盒 [x-8,x+8) x [y-8,y+8) 覆盖的最多 4 个瓦片。
    // 原 MAP_IsBlockingByPixel 的 1/2/4 点采样假设至少一个轴 8px 对齐，非对齐时
    // 会漏掉一侧瓦片，因此这里直接按瓦片完整检查。
    const int32_t x0 = (x - 8) >> 4;
    const int32_t x1 = (x + 7) >> 4;
    const int32_t y0 = (y - 8) >> 4;
    const int32_t y1 = (y + 7) >> 4;
    for (int32_t ty = y0; ty <= y1; ++ty) {
        for (int32_t tx = x0; tx <= x1; ++tx) {
            if (tile_blocking(tx, ty) != 0) return 1;
        }
    }
    return 0;
}

bool set_camera_focus(int32_t x, int32_t y) {
    const SetFocusFn fn = set_focus_fn();
    if (fn == nullptr) return false;
    fn(x, y);
    return true;
}

void* virtual_control(int32_t id) {
    const VirtualKeyRootFn root_fn = virtual_key_root_fn();
    const FindControlByIdFn find_fn = find_control_by_id_fn();
    if (root_fn == nullptr || find_fn == nullptr) return nullptr;
    void* root = root_fn();
    if (root == nullptr) return nullptr;
    return find_fn(root, id);
}

void* virtual_pad_control() {
    return virtual_control(0x15);
}

bool control_rect(void* ctrl, int64_t rect[4]) {
    if (ctrl == nullptr || rect == nullptr) return false;
    const auto* base = static_cast<const uint8_t*>(ctrl) + 0x18;
    if (!readable(base, sizeof(int64_t) * 4)) return false;
    const auto* values = reinterpret_cast<const int64_t*>(base);
    for (int i = 0; i < 4; ++i) rect[i] = values[i];
    return true;
}

bool set_control_rect(void* ctrl, int64_t x, int64_t y, int64_t w, int64_t h) {
    if (ctrl == nullptr) return false;
    auto* base = static_cast<uint8_t*>(ctrl) + 0x18;
    if (!writable(base, sizeof(int64_t) * 4)) return false;
    auto* values = reinterpret_cast<int64_t*>(base);
    values[0] = x;
    values[1] = y;
    values[2] = w;
    values[3] = h;
    return true;
}

}  // namespace qol::game::input::access
