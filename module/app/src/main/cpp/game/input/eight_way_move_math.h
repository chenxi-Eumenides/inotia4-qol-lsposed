#pragma once

// 八向/全向触屏方向数学（纯逻辑，可独立单测）。
//
// 坐标约定：dx = 触点相对摇杆中心的水平偏移，dy = 相对中心的上方向偏移（上为正）。
// 方向编号与原版 CHAR_Move 一致：0=下、1=左、2=上、3=右。
// 扇区编号：0=N、1=NE、2=E、3=SE、4=S、5=SW、6=W、7=NW。

#include <cstdint>

namespace qol::game::input {

constexpr uint32_t kCustomDirKeyBase = 0x51000000u;
constexpr int32_t kDirectionDeadzonePixels = 8;

// tan(67.5°) * 1000，用于 22.5°/67.5° 的八向边界。
constexpr int64_t kDirectionRatioThreshold = 2414;

struct EightWayClass {
    bool valid = false;
    int32_t sector = -1;
    int32_t primary_dir = -1;          // 0..3
    int32_t secondary_dir = -1;        // 0..3，-1=无
    int32_t primary_key_index = -1;    // KEY 表下标：左0/右1/上2/下3
    bool diagonal = false;
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

// 八向分类。45° 平局按原版规则归水平主轴。
inline EightWayClass classify_vector(int32_t dx, int32_t dy) {
    EightWayClass out;
    if (inside_deadzone(dx, dy)) return out;

    const int64_t ax = abs32(dx);
    const int64_t ay = abs32(dy);

    const bool horizontal =
        ax * 1000 >= ay * kDirectionRatioThreshold;
    const bool vertical =
        ay * 1000 >= ax * kDirectionRatioThreshold;

    if (horizontal && !vertical) {
        out.valid = true;
        out.sector = dx > 0 ? 2 : 6;  // E / W
        out.primary_dir = dx > 0 ? 3 : 1;
        out.primary_key_index = dx > 0 ? 1 : 0;
        return out;
    }
    if (vertical && !horizontal) {
        out.valid = true;
        out.sector = dy > 0 ? 0 : 4;  // N / S
        out.primary_dir = dy > 0 ? 2 : 0;
        out.primary_key_index = dy > 0 ? 2 : 3;
        return out;
    }

    // 斜向扇区：主轴决定主方向，另一轴做副方向。
    out.valid = true;
    out.diagonal = true;
    if (dx > 0 && dy > 0) {
        out.sector = 1;  // NE
        if (ax >= ay) {
            out.primary_dir = 3;
            out.secondary_dir = 2;
            out.primary_key_index = 1;
        } else {
            out.primary_dir = 2;
            out.secondary_dir = 3;
            out.primary_key_index = 2;
        }
    } else if (dx > 0 && dy < 0) {
        out.sector = 3;  // SE
        if (ax >= ay) {
            out.primary_dir = 3;
            out.secondary_dir = 0;
            out.primary_key_index = 1;
        } else {
            out.primary_dir = 0;
            out.secondary_dir = 3;
            out.primary_key_index = 3;
        }
    } else if (dx < 0 && dy > 0) {
        out.sector = 7;  // NW
        if (ax >= ay) {
            out.primary_dir = 1;
            out.secondary_dir = 2;
            out.primary_key_index = 0;
        } else {
            out.primary_dir = 2;
            out.secondary_dir = 1;
            out.primary_key_index = 2;
        }
    } else if (dx < 0 && dy < 0) {
        out.sector = 5;  // SW
        if (ax >= ay) {
            out.primary_dir = 1;
            out.secondary_dir = 0;
            out.primary_key_index = 0;
        } else {
            out.primary_dir = 0;
            out.secondary_dir = 1;
            out.primary_key_index = 3;
        }
    }
    // dx 或 dy 为 0 时不会进入斜向分支；保留 valid=false 的防御态。
    if (out.sector < 0) out.valid = false;
    return out;
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

// 斜向时优先保持角色当前朝向：从右进入右上/右下仍朝右，从上进入右上/左上仍朝上。
// 当前朝向不是该斜向的两个轴之一时，保留主轴判定。
inline void apply_facing_preference(EightWayClass& classified, int32_t facing) {
    if (!classified.valid || !classified.diagonal) return;
    if (facing < 0 || facing > 3) return;
    if (facing == classified.primary_dir) return;
    if (facing != classified.secondary_dir) return;
    const int32_t previous_primary = classified.primary_dir;
    classified.primary_dir = facing;
    classified.secondary_dir = previous_primary;
    classified.primary_key_index = key_index_for_dir(facing);
}

inline bool is_custom_dir_key(int32_t key) {
    const uint32_t value = static_cast<uint32_t>(key);
    return (value & 0xFFFF0000u) == kCustomDirKeyBase && (value & 0x0000FFF8u) == 0;
}

inline int32_t custom_dir_key(int32_t sector) {
    return static_cast<int32_t>(kCustomDirKeyBase | (static_cast<uint32_t>(sector) & 7u));
}

inline bool should_apply_secondary(bool enabled, bool active, bool trusted, int32_t flag,
                                   int32_t dir, int32_t primary_dir, int32_t secondary_dir) {
    return enabled && active && trusted && flag == 0 && secondary_dir >= 0 &&
           secondary_dir <= 3 && dir == primary_dir;
}

}  // namespace qol::game::input
