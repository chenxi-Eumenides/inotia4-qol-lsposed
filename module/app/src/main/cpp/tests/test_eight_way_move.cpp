#include "game/input/eight_way_move_math.h"

#include <cmath>
#include <cstdint>
#include <cstdio>

namespace {

int failures = 0;

void check(bool condition, const char* what) {
    if (condition) return;
    std::printf("FAIL: %s\n", what);
    ++failures;
}

using qol::game::input::kLatticeStepCount;
using qol::game::input::kLatticeSteps;
using qol::game::input::custom_dir_key;
using qol::game::input::is_custom_dir_key;
using qol::game::input::lattice_step_components;
using qol::game::input::scale_component;
using qol::game::input::select_lattice_step;

}  // namespace

int main() {
    check(kLatticeStepCount == 44, "lattice has 44 unique directions");
    for (int32_t i = 0; i < kLatticeStepCount; ++i) {
        const double distance = std::sqrt(static_cast<double>(kLatticeSteps[i].dx) *
                                              kLatticeSteps[i].dx +
                                          static_cast<double>(kLatticeSteps[i].dy) *
                                              kLatticeSteps[i].dy);
        check(distance >= 7.5 && distance <= 8.5, "lattice distance within 8px +/- 0.5");
        check(distance > 0.0, "lattice vector nonzero");
    }

    check(select_lattice_step(100, 0, -1) == 0, "east -> (8,0)");
    check(select_lattice_step(0, 100, -1) == 11, "north -> (0,8)");
    check(select_lattice_step(-100, 0, -1) == 22, "west -> (-8,0)");
    check(select_lattice_step(0, -100, -1) == 33, "south -> (0,-8)");
    check(select_lattice_step(5, 5, -1) == -1, "deadzone invalid");
    check(select_lattice_step(0, 0, -1) == -1, "zero invalid");
    check(select_lattice_step(8, 0, -1) == 0, "deadzone edge valid");

    {
        const int32_t index = select_lattice_step(100, 100, -1);
        const int32_t ax = kLatticeSteps[index].dx < 0 ? -kLatticeSteps[index].dx
                                                        : kLatticeSteps[index].dx;
        const int32_t ay = kLatticeSteps[index].dy < 0 ? -kLatticeSteps[index].dy
                                                        : kLatticeSteps[index].dy;
        check(index >= 0 && ((ax == 6 && ay == 5) || (ax == 5 && ay == 6)),
              "45deg uses nearest lattice point");
    }

    {
        const int32_t previous = select_lattice_step(100, 25, -1);  // 14deg, (8,2)
        check(previous == 2, "14deg -> (8,2)");
        const int32_t hysteresis = select_lattice_step(100, 28, previous);  // ~15.6deg
        check(hysteresis == previous, "hysteresis keeps previous within 4deg");
        const int32_t switched = select_lattice_step(100, 36, previous);  // ~19.8deg
        check(switched != previous, "hysteresis switches beyond 4deg");
    }

    {
        int32_t primary = -1;
        int32_t secondary = -1;
        int32_t primary_step = 0;
        int32_t secondary_step = 0;
        int32_t key_index = -1;
        check(lattice_step_components(0, -1, &primary, &secondary, &primary_step,
                                      &secondary_step, &key_index),
              "east components");
        check(primary == 3 && secondary == -1 && primary_step == 8 && secondary_step == 0 &&
                  key_index == 1,
              "east dir/step");
    }
    {
        int32_t primary = -1;
        int32_t secondary = -1;
        int32_t primary_step = 0;
        int32_t secondary_step = 0;
        int32_t key_index = -1;
        check(lattice_step_components(11, 3, &primary, &secondary, &primary_step,
                                      &secondary_step, &key_index),
              "north components");
        check(primary == 2 && secondary == -1 && primary_step == 8 && secondary_step == 0 &&
                  key_index == 2,
              "north dir/step");
    }
    {
        int32_t primary = -1;
        int32_t secondary = -1;
        int32_t primary_step = 0;
        int32_t secondary_step = 0;
        int32_t key_index = -1;
        check(lattice_step_components(5, 2, &primary, &secondary, &primary_step,
                                      &secondary_step, &key_index),
              "6/5 components facing up");
        check(primary == 2 && secondary == 3 && primary_step == 5 && secondary_step == 6 &&
                  key_index == 2,
              "6/5 keeps up facing");
    }
    {
        int32_t primary = -1;
        int32_t secondary = -1;
        int32_t primary_step = 0;
        int32_t secondary_step = 0;
        int32_t key_index = -1;
        check(lattice_step_components(6, 3, &primary, &secondary, &primary_step,
                                      &secondary_step, &key_index),
              "5/6 components facing right");
        check(primary == 3 && secondary == 2 && primary_step == 5 && secondary_step == 6 &&
                  key_index == 1,
              "5/6 keeps right facing");
    }

    check(scale_component(8, 8) == 8, "scale 8@8");
    check(scale_component(6, 8) == 6, "scale 6@8");
    check(scale_component(6, 6) == 5, "scale 6@6 rounds");
    check(scale_component(8, 1) == 1, "scale 8@1");
    check(scale_component(1, 1) == 0, "scale 1@1 floors");
    check(scale_component(0, 8) == 0, "scale zero");

    check(is_custom_dir_key(custom_dir_key(0)), "custom key 0");
    check(is_custom_dir_key(custom_dir_key(7)), "custom key 7");
    check(!is_custom_dir_key(0x51000008), "custom key out of sector range");
    check(!is_custom_dir_key(0x1234), "non custom key");

    if (failures != 0) {
        std::printf("%d failure(s)\n", failures);
        return 1;
    }
    std::printf("eight_way_move_math: ok\n");
    return 0;
}
