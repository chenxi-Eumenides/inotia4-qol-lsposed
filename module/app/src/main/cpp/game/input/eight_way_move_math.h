#pragma once

// 主控触屏方向候选点与步长数学（纯逻辑，可独立单测）。
//
// 坐标约定：dx = 触点相对摇杆中心的水平偏移，dy = 相对中心的上方向偏移（上为正）。
// 方向编号与原版 CHAR_Move 一致：0=下、1=左、2=上、3=右。
//
// 候选点不再使用固定 8px 表：基础步长由 better_move 的 better_move.step 档位提供，
// 游戏层按 (模式, 步长) 在首次触摸时计算并缓存。八向取轴向 S 与 45° 射线上最接近 S 的整数点；
// 全向取所有到原点距离四舍五入等于 S 的整数格点，按角度排序。步长越大，格点越多，
// 可用方位也越多。

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>

namespace qol::game::input {

constexpr uint32_t kCustomDirKeyBase = 0x51000000u;
constexpr int32_t kDirectionDeadzonePixels = 8;
constexpr double kLatticeHysteresisDegrees = 4.0;
constexpr double kPi = 3.14159265358979323846;
// 设置页移速档位上限 500% -> 40；留出余量防止能力返回值异常时无限扩表。
constexpr int32_t kMaxMoveStep = 64;
// r=64 的整数格点约 402 个；容量足够覆盖当前全部倍率档位。
constexpr int32_t kMaxCandidates = 512;

struct LatticeStep {
    int32_t dx = 0;  // 右为正
    int32_t dy = 0;  // 上为正
};

struct Candidate {
    int32_t dx = 0;
    int32_t dy = 0;
    double angle = 0.0;  // [0, 2*pi)，atan2(dy, dx)
};

struct CandidateSet {
    bool eight_way = false;
    int32_t step = 0;
    int32_t count = 0;
    Candidate items[kMaxCandidates];
};

inline int32_t abs32(int32_t value) {
    return value < 0 ? static_cast<int32_t>(-static_cast<int64_t>(value)) : value;
}

inline bool inside_deadzone(int32_t dx, int32_t dy) {
    const int64_t x = dx;
    const int64_t y = dy;
    return x * x + y * y <
           static_cast<int64_t>(kDirectionDeadzonePixels) * kDirectionDeadzonePixels;
}

inline double normalize_angle(double angle) {
    while (angle < 0.0) angle += 2.0 * kPi;
    while (angle >= 2.0 * kPi) angle -= 2.0 * kPi;
    return angle;
}

inline int32_t clamp_move_step(int32_t step) {
    if (step < 1) return 1;
    if (step > kMaxMoveStep) return kMaxMoveStep;
    return step;
}

// 八向：轴向为 S，对角为 45° 射线上距离 S 最近的整数点 k=round(S/sqrt(2))。
// 全向：枚举所有 round(hypot(x,y))==S 的整数点；这些点就是半径 S 圆周上最近的格点。
// 结果按角度升序排列，选中点可直接用于指针旋转，不需要每帧反算角度。
inline void build_candidate_set(bool eight_way, int32_t step, CandidateSet* out) {
    if (out == nullptr) return;
    out->eight_way = eight_way;
    out->step = clamp_move_step(step);
    out->count = 0;

    if (eight_way) {
        const int32_t diagonal = std::max(
            1, static_cast<int32_t>(std::lround(static_cast<double>(out->step) / std::sqrt(2.0))));
        const LatticeStep vectors[8] = {
            {out->step, 0},
            {diagonal, diagonal},
            {0, out->step},
            {-diagonal, diagonal},
            {-out->step, 0},
            {-diagonal, -diagonal},
            {0, -out->step},
            {diagonal, -diagonal},
        };
        for (int32_t i = 0; i < 8; ++i) {
            Candidate& candidate = out->items[out->count++];
            candidate.dx = vectors[i].dx;
            candidate.dy = vectors[i].dy;
            candidate.angle = static_cast<double>(i) * kPi / 4.0;
        }
        return;
    }

    const int32_t limit = out->step;
    for (int32_t x = -limit; x <= limit; ++x) {
        for (int32_t y = -limit; y <= limit; ++y) {
            if (x == 0 && y == 0) continue;
            const double distance = std::sqrt(static_cast<double>(x) * x +
                                              static_cast<double>(y) * y);
            if (std::llround(distance) != out->step) continue;
            if (out->count >= kMaxCandidates) break;
            Candidate& candidate = out->items[out->count++];
            candidate.dx = x;
            candidate.dy = y;
            candidate.angle = normalize_angle(std::atan2(static_cast<double>(y),
                                                         static_cast<double>(x)));
        }
        if (out->count >= kMaxCandidates) break;
    }

    std::sort(out->items, out->items + out->count,
              [](const Candidate& a, const Candidate& b) { return a.angle < b.angle; });
}

// 最近候选点；previous_index 只在同一 (模式, 步长) 缓存内有效，由调用方保证。
inline int32_t select_candidate(const CandidateSet& set, int32_t dx, int32_t dy,
                                int32_t previous_index) {
    if (set.count <= 0 || inside_deadzone(dx, dy)) return -1;

    const double angle = normalize_angle(std::atan2(static_cast<double>(dy),
                                                    static_cast<double>(dx)));
    int32_t best_index = -1;
    double best_diff = 1e9;
    for (int32_t i = 0; i < set.count; ++i) {
        const double diff = std::fabs(std::remainder(angle - set.items[i].angle, 2.0 * kPi));
        if (diff < best_diff) {
            best_diff = diff;
            best_index = i;
        }
    }

    if (previous_index >= 0 && previous_index < set.count) {
        const double previous_diff =
            std::fabs(std::remainder(angle - set.items[previous_index].angle, 2.0 * kPi));
        if (previous_diff <= kLatticeHysteresisDegrees * kPi / 180.0) {
            return previous_index;
        }
    }
    return best_index;
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

// 把候选点拆成原版 CHAR_Move 的主/副方向和步长。主轴取较大分量；两个分量接近时
// 优先保持当前四方向朝向，避免近斜向在切换时频繁翻转。
inline bool step_components(int32_t dx, int32_t dy, int32_t facing, int32_t* primary_dir,
                            int32_t* secondary_dir, int32_t* primary_step,
                            int32_t* secondary_step, int32_t* primary_key_index) {
    if (primary_dir == nullptr || secondary_dir == nullptr || primary_step == nullptr ||
        secondary_step == nullptr || primary_key_index == nullptr) {
        return false;
    }

    const int32_t ax = abs32(dx);
    const int32_t ay = abs32(dy);
    if (ax == 0 && ay == 0) return false;

    int32_t dir_x = -1;
    int32_t dir_y = -1;
    if (ax > 0) dir_x = dx > 0 ? 3 : 1;  // right / left
    if (ay > 0) dir_y = dy > 0 ? 2 : 0;  // up / down

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

inline bool step_components(LatticeStep step, int32_t facing, int32_t* primary_dir,
                            int32_t* secondary_dir, int32_t* primary_step,
                            int32_t* secondary_step, int32_t* primary_key_index) {
    return step_components(step.dx, step.dy, facing, primary_dir, secondary_dir, primary_step,
                           secondary_step, primary_key_index);
}

inline bool step_components(const Candidate& candidate, int32_t facing, int32_t* primary_dir,
                            int32_t* secondary_dir, int32_t* primary_step,
                            int32_t* secondary_step, int32_t* primary_key_index) {
    return step_components(candidate.dx, candidate.dy, facing, primary_dir, secondary_dir,
                           primary_step, secondary_step, primary_key_index);
}

// 候选点按基础步长 S 计算；游戏 delta 变化时按 delta/8 缩放，和原版 8px 语义一致。
// max_component 是缩放后的上限（S*delta/8），避免 S>8 时被旧的 delta 上限截断。
inline int32_t scaled_step_ceiling(int32_t step, int32_t delta) {
    if (step <= 0 || delta <= 0) return 0;
    return static_cast<int32_t>((static_cast<int64_t>(step) * delta + 4) / 8);
}

inline int32_t scale_component(int32_t component, int32_t delta, int32_t max_component) {
    if (component <= 0 || delta <= 0) return 0;
    int32_t scaled = static_cast<int32_t>((static_cast<int64_t>(component) * delta + 4) / 8);
    if (scaled > max_component) scaled = max_component;
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
