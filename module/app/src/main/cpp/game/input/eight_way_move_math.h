#pragma once

// 主控角色 1px 全向移动数学（纯逻辑，可独立单测）。
//
// 坐标约定：dx = 触点相对摇杆中心的水平偏移，dy = 相对中心的上方向偏移（上为正）。
// 方向编号与原版 CHAR_Move 一致：0=下、1=左、2=上、3=右。
// 表项是 8px 半径中点圆落在 1px 网格上的 44 个格点，单帧位移 7.62~8.25px。

#include <cmath>
#include <cstdint>

namespace qol::game::input {

constexpr uint32_t kCustomDirKeyBase = 0x51000000u;
constexpr int32_t kDirectionDeadzonePixels = 8;
constexpr double kLatticeHysteresisDegrees = 4.0;
constexpr double kPi = 3.14159265358979323846;

struct LatticeStep {
    int32_t dx = 0;  // 右为正
    int32_t dy = 0;  // 上为正
};

// 标准中点圆 r=8 的四象限格点，按角度排序。距离范围 7.62~8.25px，
// 最大角度间隔约 10.4°；45° 没有整数格点，最近为 39.8°/50.2°。
inline constexpr LatticeStep kLatticeSteps[] = {
    {8, 0},   {8, 1},   {8, 2},   {7, 3},   {7, 4},   {6, 5},
    {5, 6},   {4, 7},   {3, 7},   {2, 8},   {1, 8},   {0, 8},
    {-1, 8},  {-2, 8},  {-3, 7},  {-4, 7},  {-5, 6},  {-6, 5},
    {-7, 4},  {-7, 3},  {-8, 2},  {-8, 1},  {-8, 0},  {-8, -1},
    {-8, -2}, {-7, -3}, {-7, -4}, {-6, -5}, {-5, -6}, {-4, -7},
    {-3, -7}, {-2, -8}, {-1, -8}, {0, -8},  {1, -8},  {2, -8},
    {3, -7},  {4, -7},  {5, -6},  {6, -5},  {7, -4},  {7, -3},
    {8, -2},  {8, -1},
};

inline constexpr int32_t kLatticeStepCount =
    static_cast<int32_t>(sizeof(kLatticeSteps) / sizeof(kLatticeSteps[0]));

inline int32_t abs32(int32_t value) {
    return value < 0 ? static_cast<int32_t>(-static_cast<int64_t>(value)) : value;
}

inline bool inside_deadzone(int32_t dx, int32_t dy) {
    const int64_t x = dx;
    const int64_t y = dy;
    return x * x + y * y <
           static_cast<int64_t>(kDirectionDeadzonePixels) * kDirectionDeadzonePixels;
}

inline int32_t key_index_for_dir(int32_t dir) {
    switch (dir) {
        case 1: return 0;  // left
        case 3: return 1;  // right
        case 2: return 2;  // up
        case 0: return 3;  // down
        default: return -1;
    }
}

// 选择与触摸向量夹角最小的 8px 格点。previous_index 用于 4° 滞回，避免在相邻
// 表项边界抖动；调用方在死区/无效时传 -1。返回表下标，-1 表示死区。
inline int32_t select_lattice_step(int32_t dx, int32_t dy, int32_t previous_index) {
    if (inside_deadzone(dx, dy)) return -1;

    const double angle = std::atan2(static_cast<double>(dy), static_cast<double>(dx));
    int32_t best_index = -1;
    double best_diff = 1e9;
    for (int32_t i = 0; i < kLatticeStepCount; ++i) {
        const double vector_angle = std::atan2(
            static_cast<double>(kLatticeSteps[i].dy),
            static_cast<double>(kLatticeSteps[i].dx));
        const double diff = std::fabs(std::remainder(angle - vector_angle, 2.0 * kPi));
        if (diff < best_diff) {
            best_diff = diff;
            best_index = i;
        }
    }

    if (previous_index >= 0 && previous_index < kLatticeStepCount) {
        const double previous_angle = std::atan2(
            static_cast<double>(kLatticeSteps[previous_index].dy),
            static_cast<double>(kLatticeSteps[previous_index].dx));
        const double previous_diff =
            std::fabs(std::remainder(angle - previous_angle, 2.0 * kPi));
        if (previous_diff <= kLatticeHysteresisDegrees * kPi / 180.0) {
            return previous_index;
        }
    }
    return best_index;
}

// 把表项拆成原版 CHAR_Move 的主/副方向和步长。主轴取较大分量；两个分量接近时
// 优先保持当前四方向朝向，避免 6/5、5/6 这类近斜向在切换时频繁翻转。
inline bool lattice_step_components(int32_t index, int32_t facing, int32_t* primary_dir,
                                    int32_t* secondary_dir, int32_t* primary_step,
                                    int32_t* secondary_step, int32_t* primary_key_index) {
    if (index < 0 || index >= kLatticeStepCount || primary_dir == nullptr ||
        secondary_dir == nullptr || primary_step == nullptr || secondary_step == nullptr ||
        primary_key_index == nullptr) {
        return false;
    }

    const LatticeStep step = kLatticeSteps[index];
    const int32_t ax = abs32(step.dx);
    const int32_t ay = abs32(step.dy);
    if (ax == 0 && ay == 0) return false;

    int32_t dir_x = -1;
    int32_t dir_y = -1;
    if (ax > 0) dir_x = step.dx > 0 ? 3 : 1;  // right / left
    if (ay > 0) dir_y = step.dy > 0 ? 2 : 0;  // up / down

    bool prefer_facing = false;
    if (facing >= 0 && facing <= 3 && ax > 0 && ay > 0 && abs32(ax - ay) <= 2) {
        prefer_facing = facing == dir_x || facing == dir_y;
    }

    if (ax > 0 && ay > 0) {
        if (prefer_facing) {
            *primary_dir = facing;
            *secondary_dir = facing == dir_x ? dir_y : dir_x;
            *primary_step = facing == dir_x ? ax : ay;
            *secondary_step = facing == dir_x ? ay : ax;
        } else if (ax >= ay) {
            *primary_dir = dir_x;
            *secondary_dir = dir_y;
            *primary_step = ax;
            *secondary_step = ay;
        } else {
            *primary_dir = dir_y;
            *secondary_dir = dir_x;
            *primary_step = ay;
            *secondary_step = ax;
        }
    } else if (ax > 0) {
        *primary_dir = dir_x;
        *secondary_dir = -1;
        *primary_step = ax;
        *secondary_step = 0;
    } else {
        *primary_dir = dir_y;
        *secondary_dir = -1;
        *primary_step = ay;
        *secondary_step = 0;
    }

    *primary_key_index = key_index_for_dir(*primary_dir);
    return *primary_key_index >= 0;
}

// 速度 buff/debuff 改变 delta 时按 delta/8 缩放表项；delta=8 时保持表值。
inline int32_t scale_component(int32_t component, int32_t delta) {
    if (component <= 0 || delta <= 0) return 0;
    if (delta == 8) return component;
    int32_t scaled = (component * delta + 4) / 8;
    if (scaled > delta) scaled = delta;
    return scaled;
}

inline bool is_custom_dir_key(int32_t key) {
    const uint32_t value = static_cast<uint32_t>(key);
    return (value & 0xFFFF0000u) == kCustomDirKeyBase && (value & 0x0000FFF8u) == 0;
}

inline int32_t custom_dir_key(int32_t sector) {
    return static_cast<int32_t>(kCustomDirKeyBase | (static_cast<uint32_t>(sector) & 7u));
}

}  // namespace qol::game::input
