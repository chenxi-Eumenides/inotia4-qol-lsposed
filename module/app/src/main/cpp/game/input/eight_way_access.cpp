#include "game/input/eight_way_access.h"

#include <cstddef>
#include <cstdint>

#include "game/access/game_access.h"
#include "game/symbols/game_symbols.h"

namespace qol::game::input::access {
namespace {

using ScreenHeightFn = int (*)();
using TouchOffsetFn = void* (*)();

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
    const auto* ptr = reinterpret_cast<const uint8_t*>(g_base + G_KEY_PRESS_ACTIVE_VMA);
    return readable(ptr, sizeof(uint8_t)) && *ptr != 0;
}

bool write_press_active(bool active) {
    if (g_base == 0) return false;
    auto* ptr = reinterpret_cast<uint8_t*>(g_base + G_KEY_PRESS_ACTIVE_VMA);
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

}  // namespace qol::game::input::access
