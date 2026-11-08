#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>

namespace qol::extension_bag {
constexpr size_t kMaxEquipBagChildren = 32;

struct NativeTabBounds {
    int64_t x = 0;
    int64_t y = 0;
    int64_t width = 0;
    int64_t height = 0;
    int64_t local_x = 0;
    int64_t local_y = 0;
    bool module_control = false;
    bool native_tab = true;
};

struct C18ReflowPlan {
    int64_t module_x = 0;
    std::array<int64_t, 5> module_y{};
    int columns = 1;
    int rendered_tabs = 5;
    int64_t frame_right = 0;
    int64_t native_clearance_x = 0;
    int64_t vertical_delta = 0;
    std::array<uint8_t, kMaxEquipBagChildren> move_vertical{};
};

inline bool c18_extension_tab_layout(
    const std::array<NativeTabBounds, kMaxEquipBagChildren>& children,
    size_t child_count, int64_t parent_x, int64_t parent_y,
    int64_t screen_width, int64_t screen_height, int rendered_tabs,
    C18ReflowPlan* out_plan) {
    const auto add_fits = [](int64_t left, int64_t right) {
        return right >= 0 ? left <= std::numeric_limits<int64_t>::max() - right
                          : left >= std::numeric_limits<int64_t>::min() - right;
    };
    const auto within_three = [](int64_t a, int64_t b) {
        return a >= b
            ? (b > std::numeric_limits<int64_t>::max() - 3 || a <= b + 3)
            : (b < std::numeric_limits<int64_t>::min() + 3 || a >= b - 3);
    };
    if (child_count > kMaxEquipBagChildren || out_plan == nullptr ||
        rendered_tabs < 1 || rendered_tabs > 20 ||
        screen_width <= 0 || screen_height <= 0) return false;

    C18ReflowPlan plan{};
    std::array<int64_t, 6> original_rows{};
    size_t original_count = 0;
    int64_t original_x = 0;
    for (size_t i = 0; i < child_count; ++i) {
        const auto& child = children[i];
        if (child.module_control || !child.native_tab || child.width <= 0 || child.height <= 0) continue;
        if (!add_fits(child.x, child.width) || !add_fits(child.y, child.height)) return false;
        if (child.width == 57 && child.height == 57) {
            if (original_count == original_rows.size()) return false;
            if (original_count == 0) original_x = child.x;
            if (child.x != original_x) return false;
            original_rows[original_count++] = child.y;
        }
    }
    if (original_count != original_rows.size()) return false;
    std::sort(original_rows.begin(), original_rows.end());
    for (size_t row = 1; row < original_rows.size(); ++row) {
        if (original_rows[row] <= original_rows[row - 1]) return false;
    }

    std::array<size_t, kMaxEquipBagChildren> forty_controls{};
    size_t forty_count = 0;
    for (size_t i = 0; i < child_count; ++i) {
        const auto& child = children[i];
        if (!child.module_control && child.native_tab && child.width == 40 && child.height == 40) {
            forty_controls[forty_count++] = i;
        }
    }

    // A non-original row repeated across five or more x positions is a fixed
    // horizontal row, not a vertical group, even when it shares one x with one.
    std::array<uint8_t, kMaxEquipBagChildren> fixed_horizontal{};
    for (size_t i = 0; i < forty_count; ++i) {
        const auto& candidate = children[forty_controls[i]];
        for (size_t j = i + 1; j < forty_count; ++j) {
            const auto& peer = children[forty_controls[j]];
            if (peer.x == candidate.x && peer.y == candidate.y) return false;
        }
        bool aligned_original_row = false;
        for (const int64_t y : original_rows) aligned_original_row |= within_three(candidate.y, y);
        if (aligned_original_row) continue;
        size_t distinct_x = 0;
        for (size_t j = 0; j < forty_count; ++j) {
            const auto& peer = children[forty_controls[j]];
            if (peer.y != candidate.y) continue;
            bool seen_x = false;
            for (size_t k = 0; k < j; ++k) {
                const auto& prior = children[forty_controls[k]];
                if (prior.y == candidate.y && prior.x == peer.x) { seen_x = true; break; }
            }
            if (!seen_x) ++distinct_x;
        }
        std::array<int64_t, kMaxEquipBagChildren> horizontal_xs{};
        size_t horizontal_count = 0;
        for (size_t j = 0; j < forty_count; ++j) {
            const auto& peer = children[forty_controls[j]];
            if (peer.y != candidate.y) continue;
            bool seen_x = false;
            for (size_t k = 0; k < horizontal_count; ++k) seen_x |= horizontal_xs[k] == peer.x;
            if (!seen_x) horizontal_xs[horizontal_count++] = peer.x;
        }
        bool regular_horizontal = distinct_x >= 5;
        if (regular_horizontal) {
            std::sort(horizontal_xs.begin(), horizontal_xs.begin() + horizontal_count);
            if (horizontal_xs[0] > std::numeric_limits<int64_t>::max() - 40 ||
                (horizontal_xs[0] < 0 &&
                 horizontal_xs[1] > std::numeric_limits<int64_t>::max() + horizontal_xs[0]) ||
                horizontal_xs[1] <= horizontal_xs[0] + 40) {
                regular_horizontal = false;
            } else {
                const int64_t horizontal_step = horizontal_xs[1] - horizontal_xs[0];
                for (size_t x_index = 2; x_index < horizontal_count; ++x_index) {
                    if (horizontal_xs[x_index - 1] >
                            std::numeric_limits<int64_t>::max() - horizontal_step ||
                        horizontal_xs[x_index] != horizontal_xs[x_index - 1] + horizontal_step) {
                        regular_horizontal = false;
                        break;
                    }
                }
            }
        }
        if (regular_horizontal) {
            for (size_t j = 0; j < forty_count; ++j) {
                if (children[forty_controls[j]].y == candidate.y) {
                    fixed_horizontal[forty_controls[j]] = 1;
                }
            }
        }
    }

    std::array<int64_t, kMaxEquipBagChildren> group_x{};
    std::array<std::array<int32_t, kMaxEquipBagChildren>, kMaxEquipBagChildren> group_indices{};
    std::array<size_t, kMaxEquipBagChildren> group_counts{};
    size_t group_count = 0;
    std::array<uint8_t, kMaxEquipBagChildren> is_vertical{};
    for (size_t n = 0; n < forty_count; ++n) {
        const size_t index = forty_controls[n];
        const auto& child = children[index];
        if (fixed_horizontal[index] != 0) continue;
        size_t group = 0;
        while (group < group_count && group_x[group] != child.x) ++group;
        if (group == group_count) {
            if (group_count == group_x.size()) return false;
            group_x[group] = child.x;
            group_indices[group].fill(-1);
            ++group_count;
        }
        if (group_counts[group] == kMaxEquipBagChildren) return false;
        group_indices[group][group_counts[group]++] = static_cast<int32_t>(index);
    }

    std::array<size_t, kMaxEquipBagChildren> vertical_groups{};
    size_t vertical_group_count = 0;
    for (size_t group = 0; group < group_count; ++group) {
        const size_t count = group_counts[group];
        bool row_aligned = false;
        for (size_t n = 0; n < count; ++n) {
            const auto& item = children[static_cast<size_t>(group_indices[group][n])];
            for (const int64_t y : original_rows) row_aligned |= within_three(item.y, y);
        }
        if (count < 5) {
            if (row_aligned) return false;
            continue;
        }
        std::sort(group_indices[group].begin(), group_indices[group].begin() + count,
            [&children](int32_t left, int32_t right) {
                const auto& a = children[static_cast<size_t>(left)];
                const auto& b = children[static_cast<size_t>(right)];
                return a.y < b.y;
            });
        for (size_t row = 1; row < count; ++row) {
            if (children[static_cast<size_t>(group_indices[group][row])].y <=
                children[static_cast<size_t>(group_indices[group][row - 1])].y) return false;
        }
        const size_t original_aligned_rows = count < 6 ? count : 6;
        for (size_t row = 0; row < original_aligned_rows; ++row) {
            const int64_t item_y = children[static_cast<size_t>(group_indices[group][row])].y;
            if (!within_three(item_y, original_rows[row])) return false;
        }
        const int64_t first_y = children[static_cast<size_t>(group_indices[group][0])].y;
        const int64_t second_y = children[static_cast<size_t>(group_indices[group][1])].y;
        if (first_y < 0 && second_y > std::numeric_limits<int64_t>::max() + first_y) return false;
        const int64_t step = second_y - first_y;
        if (step <= 0) return false;
        for (size_t row = 1; row < count; ++row) {
            const int64_t previous = children[static_cast<size_t>(group_indices[group][row - 1])].y;
            const int64_t current = children[static_cast<size_t>(group_indices[group][row])].y;
            if (previous > std::numeric_limits<int64_t>::max() - step || current != previous + step) {
                return false;
            }
        }
        vertical_groups[vertical_group_count++] = group;
    }

    // Any type-2 control aligned to an original row must belong to a complete
    // repeated-x group; otherwise its role is ambiguous and no reflow is safe.
    for (size_t n = 0; n < forty_count; ++n) {
        const size_t index = forty_controls[n];
        const auto& item = children[index];
        bool aligned = false;
        for (const int64_t y : original_rows) aligned |= within_three(item.y, y);
        if (!aligned || fixed_horizontal[index] != 0) continue;
        bool belongs = false;
        for (size_t v = 0; v < vertical_group_count; ++v) {
            const size_t group = vertical_groups[v];
            for (size_t row = 0; row < group_counts[group]; ++row) {
                belongs |= group_indices[group][row] == static_cast<int32_t>(index);
            }
        }
        if (!belongs) return false;
    }

    for (size_t v = 1; v < vertical_group_count; ++v) {
        const size_t first_group = vertical_groups[0];
        const size_t group = vertical_groups[v];
        if (group_counts[group] != group_counts[first_group]) return false;
        for (size_t row = 0; row < group_counts[group]; ++row) {
            const auto& a = children[static_cast<size_t>(group_indices[first_group][row])];
            const auto& b = children[static_cast<size_t>(group_indices[group][row])];
            if (a.y != b.y) return false;
        }
    }

    if (original_x > std::numeric_limits<int64_t>::max() - 67) return false;
    const int64_t module_abs_x = original_x + 67; // 5px original-column to frame clearance.
    const int64_t frame_left = module_abs_x - 5;
    const int columns = (rendered_tabs + 4) / 5;
    const int64_t column_offset = static_cast<int64_t>(columns - 1) * 68;
    if (!add_fits(module_abs_x, column_offset) ||
        !add_fits(module_abs_x + column_offset, 67)) return false;
    const int64_t frame_right = module_abs_x + column_offset + 67;
    if (frame_right > std::numeric_limits<int64_t>::max() - 5) return false;
    const int64_t native_clearance_x = frame_right + 5;
    plan.frame_right = frame_right;
    plan.native_clearance_x = native_clearance_x;
    for (int row = 0; row < 5; ++row) {
        const int64_t tab_y = original_rows[static_cast<size_t>(row)];
        if (tab_y < std::numeric_limits<int64_t>::min() + 12) return false;
        const int64_t frame_y = tab_y - 12;
        if (!add_fits(frame_y, 84) || frame_left < 0 || frame_y < 0 ||
            frame_y + 84 > screen_height ||
            tab_y < parent_y ||
            (parent_y < 0 && tab_y > std::numeric_limits<int64_t>::max() + parent_y)) return false;
        plan.module_y[static_cast<size_t>(row)] = tab_y - parent_y;
    }
    plan.columns = columns;
    plan.rendered_tabs = rendered_tabs;
    if (module_abs_x < parent_x ||
        (parent_x < 0 && module_abs_x > std::numeric_limits<int64_t>::max() + parent_x)) return false;
    plan.module_x = module_abs_x - parent_x;

    bool conflict = false;
    int64_t minimum_vertical_x = std::numeric_limits<int64_t>::max();
    for (size_t v = 0; v < vertical_group_count; ++v) {
        const size_t group = vertical_groups[v];
        for (size_t row = 0; row < group_counts[group]; ++row) {
            const size_t index = static_cast<size_t>(group_indices[group][row]);
            const auto& native = children[index];
            is_vertical[index] = 1;
            minimum_vertical_x = std::min(minimum_vertical_x, native.x);
            if (native.x < native_clearance_x && native.x + native.width > frame_left) conflict = true;
        }
    }
    for (size_t i = 0; i < child_count; ++i) {
        const auto& child = children[i];
        if (!child.native_tab || child.module_control || is_vertical[i] != 0 ||
            child.width <= 0 || child.height <= 0) continue;
        for (size_t row = 0; row < 5; ++row) {
            const int64_t frame_y = original_rows[row] - 12;
            if (child.x < native_clearance_x && child.x + child.width > frame_left &&
                child.y < frame_y + 84 && child.y + child.height > frame_y) return false;
        }
    }
    if (conflict) {
        if (vertical_group_count == 0 || minimum_vertical_x > native_clearance_x) return false;
        plan.vertical_delta = native_clearance_x - minimum_vertical_x;
        for (size_t i = 0; i < child_count; ++i) {
            if (is_vertical[i] == 0) continue;
            const auto& child = children[i];
            if (!add_fits(child.x, plan.vertical_delta) ||
                child.x + plan.vertical_delta < 0 ||
                child.x + plan.vertical_delta > screen_width - child.width) return false;
            plan.move_vertical[i] = 1;
        }
    }
    *out_plan = plan;
    return true;
}
}  // namespace qol::extension_bag
