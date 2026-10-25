#include "plugins/extension_bag/core/equip_tab_layout.h"

#include <cassert>
#include <cstdint>
#include <initializer_list>

using qol::extension_bag::C18ReflowPlan;
using qol::extension_bag::NativeTabBounds;
using qol::extension_bag::kMaxEquipBagChildren;

namespace {
struct Fixture {
    std::array<NativeTabBounds, kMaxEquipBagChildren> children{};
    size_t count = 0;
    int64_t parent_x = 200;
    int64_t parent_y = 50;
};

Fixture make_v513(int64_t vertical_x1 = 254, int64_t vertical_x2 = 308) {
    Fixture fixture{};
    constexpr std::array<int64_t, 6> original_y{50, 120, 189, 258, 327, 397};
    constexpr std::array<int64_t, 6> c18_y{50, 120, 190, 260, 330, 400};
    for (size_t row = 0; row < 6; ++row) {
        fixture.children[fixture.count++] = {200, original_y[row], 57, 57, 0, 0, false, true};
    }
    for (const int64_t x : {vertical_x1, vertical_x2}) {
        for (const int64_t y : c18_y) {
            fixture.children[fixture.count++] = {x, y, 40, 40, 0, 0, false, true};
        }
    }
    for (int i = 0; i < 5; ++i) {
        fixture.children[fixture.count++] = {0, 0, 0, 0, 0, 0, true, false};
    }
    return fixture;
}

Fixture make_v514() {
    Fixture fixture{};
    constexpr std::array<int64_t, 6> original_y{50, 120, 189, 258, 327, 397};
    constexpr std::array<int64_t, 6> vertical_y{50, 120, 190, 260, 330, 400};
    for (size_t row = 0; row < 6; ++row) {
        fixture.children[fixture.count++] = {200, original_y[row], 57, 57, 0, 0, false, true};
    }
    for (const int64_t y : vertical_y) {
        fixture.children[fixture.count++] = {264, y, 40, 40, 0, 0, false, true};
    }
    constexpr std::array<int64_t, 6> bottom_x{-126, -48, 30, 108, 186, 264};
    for (const int64_t x : bottom_x) {
        fixture.children[fixture.count++] = {x, 470, 40, 40, 0, 0, false, true};
    }
    for (int i = 0; i < 5; ++i) {
        fixture.children[fixture.count++] = {0, 0, 0, 0, 0, 0, true, false};
    }
    return fixture;
}

Fixture make_single_vertical_group(size_t row_count) {
    Fixture fixture{};
    constexpr std::array<int64_t, 6> original_y{50, 120, 189, 258, 327, 397};
    for (const int64_t y : original_y) {
        fixture.children[fixture.count++] = {200, y, 57, 57, 0, 0, false, true};
    }
    for (size_t row = 0; row < row_count; ++row) {
        fixture.children[fixture.count++] = {254, 50 + static_cast<int64_t>(row) * 70,
                                             40, 40, 0, 0, false, true};
    }
    for (int i = 0; i < 5; ++i) {
        fixture.children[fixture.count++] = {0, 0, 0, 0, 0, 0, true, false};
    }
    return fixture;
}

Fixture add_fixed_row(Fixture fixture, int64_t first_x) {
    for (int i = 0; i < 5; ++i) {
        fixture.children[fixture.count++] = {first_x + i * 78, 110, 40, 40, 0, 0, false, true};
    }
    return fixture;
}

bool plan(Fixture& fixture, C18ReflowPlan* out) {
    return qol::extension_bag::c18_extension_tab_layout(fixture.children, fixture.count,
        fixture.parent_x, fixture.parent_y, 960, 640, out);
}
}  // namespace

int main() {
    C18ReflowPlan plan513{};
    auto v513 = make_v513();
    assert(plan(v513, &plan513));
    assert(plan513.module_x == 67);
    assert((plan513.module_y == std::array<int64_t, 5>{0, 70, 139, 208, 277}));
    assert(plan513.vertical_delta == 85);
    const int64_t original_right = 200 + 57;
    const int64_t module_frame_left = 200 + plan513.module_x - 5;
    const int64_t module_frame_right = module_frame_left + 72;
    assert(module_frame_left - original_right == 5);
    assert((254 + plan513.vertical_delta) - module_frame_right == 5);
    for (size_t i = 6; i < 18; ++i) assert(plan513.move_vertical[i] == 1);
    for (size_t i = 18; i < v513.count; ++i) assert(plan513.move_vertical[i] == 0);

    // A clear slot after the original column means no native rect is changed.
    C18ReflowPlan clear_plan{};
    auto clear = make_v513(339, 393);
    assert(plan(clear, &clear_plan));
    assert(clear_plan.vertical_delta == 0);
    for (const uint8_t move : clear_plan.move_vertical) assert(move == 0);
    C18ReflowPlan four_pixel_gap{};
    auto near_clear = make_v513(338, 392);
    assert(plan(near_clear, &four_pixel_gap));
    assert(four_pixel_gap.vertical_delta == 1);

    // Fixed native rows are never moved; a 1-4px frame clearance rejects,
    // while the exact 5px clearance remains available.
    auto fixed_gap4 = add_fixed_row(make_v513(), 338);
    C18ReflowPlan fixed_gap_plan{};
    assert(!plan(fixed_gap4, &fixed_gap_plan));
    auto fixed_gap5 = add_fixed_row(make_v513(), 339);
    assert(plan(fixed_gap5, &fixed_gap_plan));
    assert(fixed_gap_plan.vertical_delta == 85);
    // v514: shift only the row-aligned vertical group; preserve all six bottom controls.
    C18ReflowPlan plan514{};
    auto v514 = make_v514();
    assert(plan(v514, &plan514));
    assert(plan514.vertical_delta == 75);
    for (size_t i = 6; i < 12; ++i) assert(plan514.move_vertical[i] == 1);
    for (size_t i = 12; i < 23; ++i) assert(plan514.move_vertical[i] == 0);

    // Repeated-x geometry detects five-row and seven-row vertical groups without variant lists.
    C18ReflowPlan five_rows{};
    auto five = make_single_vertical_group(5);
    assert(plan(five, &five_rows));
    for (size_t i = 6; i < 11; ++i) assert(five_rows.move_vertical[i] == 1);
    C18ReflowPlan seven_rows{};
    auto seven = make_single_vertical_group(7);
    assert(plan(seven, &seven_rows));
    for (size_t i = 6; i < 13; ++i) assert(seven_rows.move_vertical[i] == 1);

    // Re-running against moved geometry is idempotent; a rebuilt tree is scanned afresh.
    for (size_t i = 6; i < 18; ++i) v513.children[i].x += plan513.vertical_delta;
    C18ReflowPlan moved_plan{};
    assert(plan(v513, &moved_plan));
    assert(moved_plan.vertical_delta == 0);
    auto rebuilt_v514 = make_v514();
    C18ReflowPlan rebuilt_plan{};
    assert(plan(rebuilt_v514, &rebuilt_plan));
    assert(rebuilt_plan.vertical_delta == 75);

    auto incomplete = make_v513();
    incomplete.children[17].native_tab = false;
    C18ReflowPlan rejected{};
    assert(!plan(incomplete, &rejected));
    auto malformed = make_v513();
    malformed.children[17].width = 57;
    assert(!plan(malformed, &rejected));
    malformed = make_v513();
    malformed.children[17].y += 4;
    assert(!plan(malformed, &rejected));
    malformed = make_v513();
    malformed.children[12].y += 1;  // Parallel C18 groups must share row coordinates.
    assert(!plan(malformed, &rejected));

    assert(!qol::extension_bag::c18_extension_tab_layout(v513.children,
        kMaxEquipBagChildren + 1, v513.parent_x, v513.parent_y, 960, 640, &rejected));
    auto too_narrow = make_v513();
    assert(!qol::extension_bag::c18_extension_tab_layout(too_narrow.children,
        too_narrow.count, too_narrow.parent_x, too_narrow.parent_y, 333, 640, &rejected));
    auto too_short = make_v513();
    assert(!qol::extension_bag::c18_extension_tab_layout(too_short.children,
        too_short.count, too_short.parent_x, too_short.parent_y, 960, 398, &rejected));
}
