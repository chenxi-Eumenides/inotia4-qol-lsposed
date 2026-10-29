#include "game/input/eight_way_move_math.h"
#include "game/input/eight_way_state.h"

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

using qol::game::input::build_candidate_set;
using qol::game::input::CandidateSet;
using qol::game::input::custom_dir_key;
using qol::game::input::is_custom_dir_key;
using qol::game::input::kPi;
using qol::game::input::scale_component;
using qol::game::input::scaled_step_ceiling;
using qol::game::input::select_candidate;
using qol::game::input::step_components;

bool same_vector(const qol::game::input::Candidate& candidate, int32_t dx, int32_t dy) {
    return candidate.dx == dx && candidate.dy == dy;
}

}  // namespace

int main() {
    CandidateSet eight;
    build_candidate_set(true, 8, &eight);
    check(eight.count == 8, "eight-way set has eight candidates");
    const int32_t expected_eight[8][2] = {
        {8, 0}, {6, 6}, {0, 8}, {-6, 6}, {-8, 0}, {-6, -6}, {0, -8}, {6, -6},
    };
    for (int32_t i = 0; i < 8; ++i) {
        check(same_vector(eight.items[i], expected_eight[i][0], expected_eight[i][1]),
              "eight-way candidate is the nearest 45-degree step");
        const double expected_angle = static_cast<double>(i) * kPi / 4.0;
        check(std::fabs(eight.items[i].angle - expected_angle) < 1e-9,
              "eight-way candidate caches its exact angle");
    }
    check(std::fabs(std::sqrt(6.0 * 6.0 + 6.0 * 6.0) - 8.485) < 0.01,
          "eight-way diagonal remains the nearest 45-degree integer point");

    CandidateSet omni;
    build_candidate_set(false, 8, &omni);
    check(omni.count == 48, "radius 8 digital circle has 48 integer points");
    bool has_diagonal = false;
    for (int32_t i = 0; i < omni.count; ++i) {
        const double distance = std::sqrt(static_cast<double>(omni.items[i].dx) *
                                              omni.items[i].dx +
                                          static_cast<double>(omni.items[i].dy) *
                                              omni.items[i].dy);
        check(std::llround(distance) == 8, "omni point is nearest to radius 8");
        check(omni.items[i].angle >= 0.0 && omni.items[i].angle < 2.0 * kPi,
              "omni angle normalized");
        if (i > 0) check(omni.items[i - 1].angle <= omni.items[i].angle,
                         "omni points sorted by angle");
        if (same_vector(omni.items[i], 6, 6)) has_diagonal = true;
    }
    check(has_diagonal, "omni set includes the 45-degree point");

    CandidateSet larger;
    build_candidate_set(false, 12, &larger);
    check(larger.count == 68, "radius 12 has more directions");
    build_candidate_set(false, 40, &larger);
    check(larger.count == 264, "radius 40 has 264 directions");
    build_candidate_set(false, 1, &larger);
    check(larger.count == 8, "radius 1 has eight neighbor directions");
    build_candidate_set(false, 2, &larger);
    check(larger.count == 12, "radius 2 has twelve directions");

    build_candidate_set(false, 8, &omni);
    check(select_candidate(omni, 100, 0, -1) == 0, "east -> first candidate");
    check(select_candidate(omni, 0, 100, -1) == 12, "north -> quarter turn");
    check(select_candidate(omni, -100, 0, -1) == 24, "west -> half turn");
    check(select_candidate(omni, 0, -100, -1) == 36, "south -> three quarter turn");
    check(select_candidate(omni, 5, 5, -1) == -1, "deadzone invalid");
    check(select_candidate(omni, 0, 0, -1) == -1, "zero invalid");
    check(select_candidate(omni, 8, 0, -1) == 0, "deadzone edge valid");

    check(select_candidate(eight, 100, 0, -1) == 0, "eight-way east");
    check(select_candidate(eight, 100, 100, -1) == 1, "eight-way northeast");
    check(select_candidate(eight, 0, 100, -1) == 2, "eight-way north");
    check(select_candidate(eight, -100, 100, -1) == 3, "eight-way northwest");
    check(select_candidate(eight, -100, -100, -1) == 5, "eight-way southwest");
    check(select_candidate(eight, 100, -100, -1) == 7, "eight-way southeast");

    {
        const int32_t previous = select_candidate(omni, 100, 25, -1);  // 14deg, (8,2)
        check(previous == 2, "14deg -> (8,2)");
        const int32_t hysteresis = select_candidate(omni, 100, 28, previous);  // ~15.6deg
        check(hysteresis == previous, "hysteresis keeps previous within 4deg");
        const int32_t switched = select_candidate(omni, 100, 36, previous);  // ~19.8deg
        check(switched != previous, "hysteresis switches beyond 4deg");
    }

    {
        int32_t primary = -1;
        int32_t secondary = -1;
        int32_t primary_step = 0;
        int32_t secondary_step = 0;
        int32_t key_index = -1;
        check(step_components(eight.items[1], -1, &primary, &secondary, &primary_step,
                              &secondary_step, &key_index),
              "eight-way diagonal components");
        check(primary == 3 && secondary == 2 && primary_step == 6 && secondary_step == 6,
              "eight-way diagonal retains main/sub-axis decomposition");
    }
    {
        int32_t primary = -1;
        int32_t secondary = -1;
        int32_t primary_step = 0;
        int32_t secondary_step = 0;
        int32_t key_index = -1;
        check(step_components(8, 0, -1, &primary, &secondary, &primary_step, &secondary_step,
                              &key_index),
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
        check(step_components(0, 8, 3, &primary, &secondary, &primary_step, &secondary_step,
                              &key_index),
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
        check(step_components(5, 6, 2, &primary, &secondary, &primary_step, &secondary_step,
                              &key_index),
              "6/5 components facing up");
        check(primary == 2 && secondary == 3 && primary_step == 6 && secondary_step == 5 &&
                  key_index == 2,
              "6/5 keeps up facing");
    }
    {
        int32_t primary = -1;
        int32_t secondary = -1;
        int32_t primary_step = 0;
        int32_t secondary_step = 0;
        int32_t key_index = -1;
        check(step_components(6, 5, 3, &primary, &secondary, &primary_step, &secondary_step,
                              &key_index),
              "5/6 components facing right");
        check(primary == 3 && secondary == 2 && primary_step == 6 && secondary_step == 5 &&
                  key_index == 1,
              "5/6 keeps right facing");
    }

    check(scale_component(8, 8, 8) == 8, "scale 8@8");
    check(scale_component(6, 8, 8) == 6, "scale 6@8");
    check(scale_component(6, 6, 6) == 5, "scale 6@6 rounds");
    check(scale_component(8, 1, 1) == 1, "scale 8@1");
    check(scale_component(1, 1, 1) == 0, "scale 1@1 floors");
    check(scale_component(0, 8, 8) == 0, "scale zero");
    check(scale_component(12, 8, 12) == 12, "step 12 is not clamped to native delta");
    check(scale_component(12, 12, 18) == 18, "step 12 with delta 12 scales to 18");
    check(scaled_step_ceiling(12, 8) == 12 && scaled_step_ceiling(12, 12) == 18 &&
              scaled_step_ceiling(8, 8) == 8,
          "scaled ceiling follows the base step");

    {
        qol::game::input::State state;
        state.active = true;
        state.owns_input = true;
        state.set_selected_candidate(qol::contracts::QolMovementInputMode::kEightWay, 1, 6, 6,
                                     kPi / 4.0, 8);
        check(state.selected_dx == 6 && state.selected_dy == 6 &&
                  std::fabs(state.selected_angle - kPi / 4.0) < 1e-9 &&
                  state.candidate_step == 8,
              "state exposes the selected vector, cached angle and step");
        state.reset_for_mode(qol::contracts::QolMovementInputMode::kOmnidirectional);
        check(!state.active && !state.owns_input && state.lattice_index == -1 &&
                  state.selected_dx == 0 && state.selected_dy == 0 &&
                  state.selected_angle == 0.0 && state.candidate_step == 0 &&
                  state.mode == qol::contracts::QolMovementInputMode::kOmnidirectional,
              "mode reset clears candidate/input state and cached angle");
    }

    check(qol::contracts::legacy_movement_input_mode(0) ==
              qol::contracts::QolMovementInputMode::kDisabled &&
              qol::contracts::legacy_movement_input_mode(1) ==
              qol::contracts::QolMovementInputMode::kOmnidirectional,
          "legacy set_enabled retains disabled/full-mode semantics");

    check(is_custom_dir_key(custom_dir_key(0)), "custom key 0");
    check(is_custom_dir_key(custom_dir_key(7)), "custom key 7");
    check(!is_custom_dir_key(0x51000008), "custom key out of sector range");
    check(!is_custom_dir_key(0x1234), "non custom key");

    if (failures != 0) {
        std::printf("%d failure(s)\n", failures);
        return 1;
    }
    std::printf("better_move_math: ok\n");
    return 0;
}
