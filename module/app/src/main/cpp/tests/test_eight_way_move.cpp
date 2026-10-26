#include "game/input/eight_way_move_math.h"

#include <cstdint>
#include <cstdio>

namespace {

int failures = 0;

void check(bool condition, const char* what) {
    if (condition) return;
    std::printf("FAIL: %s\n", what);
    ++failures;
}

using qol::game::input::EightWayClass;
using qol::game::input::classify_vector;
using qol::game::input::custom_dir_key;
using qol::game::input::is_custom_dir_key;
using qol::game::input::should_apply_secondary;

void check_class(const char* name, int32_t dx, int32_t dy, int32_t sector,
                 int32_t primary, int32_t secondary, int32_t key_index, bool diagonal) {
    const EightWayClass c = classify_vector(dx, dy);
    if (!c.valid || c.sector != sector || c.primary_dir != primary ||
        c.secondary_dir != secondary || c.primary_key_index != key_index ||
        c.diagonal != diagonal) {
        std::printf(
            "FAIL: %s dx=%d dy=%d -> valid=%d sector=%d primary=%d secondary=%d key=%d "
            "diag=%d\n",
            name, dx, dy, c.valid ? 1 : 0, c.sector, c.primary_dir, c.secondary_dir,
            c.primary_key_index, c.diagonal ? 1 : 0);
        ++failures;
    }
}

}  // namespace

int main() {
    check_class("E", 100, 0, 2, 3, -1, 1, false);
    check_class("W", -100, 0, 6, 1, -1, 0, false);
    check_class("N", 0, 100, 0, 2, -1, 2, false);
    check_class("S", 0, -100, 4, 0, -1, 3, false);

    check_class("NE tie", 100, 100, 1, 3, 2, 1, true);
    check_class("NE vertical", 60, 100, 1, 2, 3, 2, true);
    check_class("NW tie", -100, 100, 7, 1, 2, 0, true);
    check_class("NW vertical", -60, 100, 7, 2, 1, 2, true);
    check_class("SE tie", 100, -100, 3, 3, 0, 1, true);
    check_class("SW tie", -100, -100, 5, 1, 0, 0, true);

    // 22.5° 边界：100:41 仍是水平，100:42 进入斜向。
    check_class("E boundary", 100, 41, 2, 3, -1, 1, false);
    check_class("NE boundary", 100, 42, 1, 3, 2, 1, true);

    check(!classify_vector(5, 5).valid, "deadzone invalid");
    check(!classify_vector(0, 0).valid, "zero invalid");
    check(classify_vector(8, 0).valid, "deadzone edge valid");

    check(is_custom_dir_key(custom_dir_key(0)), "custom key 0");
    check(is_custom_dir_key(custom_dir_key(7)), "custom key 7");
    check(!is_custom_dir_key(0x51000008), "custom key out of sector range");
    check(!is_custom_dir_key(0x1234), "non custom key");

    check(should_apply_secondary(true, true, true, 0, 3, 3, 2), "apply secondary");
    check(!should_apply_secondary(true, true, true, 0, 2, 3, 2), "dir mismatch");
    check(!should_apply_secondary(true, true, false, 0, 3, 3, 2), "not trusted");
    check(!should_apply_secondary(true, true, true, 1, 3, 3, 2), "flag nonzero");
    check(!should_apply_secondary(true, false, true, 0, 3, 3, 2), "not active");
    check(!should_apply_secondary(false, true, true, 0, 3, 3, 2), "disabled");
    check(!should_apply_secondary(true, true, true, 0, 3, 3, -1), "no secondary");

    if (failures != 0) {
        std::printf("%d failure(s)\n", failures);
        return 1;
    }
    std::printf("eight_way_move_math: ok\n");
    return 0;
}
