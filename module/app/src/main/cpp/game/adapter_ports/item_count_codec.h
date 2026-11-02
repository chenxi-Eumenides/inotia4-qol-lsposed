#pragma once

#include <cstdint>

// S2 数量编码（a 段 bits22–24、b 段 bits25–31，数量 = 128a+b）与读视图/写上限的纯转换。
// 关闭态读视图只取 b（0..127）；写转换仍受 99 上限约束。adapter 私有实现：插件不得
// include 本头，也不持有编码布局。所有函数为 constexpr，不读游戏对象、不依赖 mode。

namespace qol::game {
namespace item_count_codec {

constexpr uint32_t kLegacyMax = 99;
constexpr uint32_t kExtendedMax = 999;

constexpr uint32_t max_count(bool extended) {
    return extended ? kExtendedMax : kLegacyMax;
}

constexpr uint32_t kS2ShiftA = 22;
constexpr uint32_t kS2BitsA = 3;
constexpr uint32_t kS2MaskA = ((1u << kS2BitsA) - 1u) << kS2ShiftA;
constexpr uint32_t kS2ShiftB = 25;
constexpr uint32_t kS2BitsB = 7;
constexpr uint32_t kS2MaskB = ((1u << kS2BitsB) - 1u) << kS2ShiftB;
constexpr uint32_t kS2Max = (1u << (kS2BitsA + kS2BitsB)) - 1u;

static_assert(kS2ShiftA + kS2BitsA == kS2ShiftB, "S2 a/b 段必须相邻");
static_assert(kS2Max == 1023u, "S2 编码容量必须为 1023");

constexpr uint32_t s2_split_a(uint32_t count) {
    return (count >> kS2BitsB) & ((1u << kS2BitsA) - 1u);
}

constexpr uint32_t s2_split_b(uint32_t count) {
    return count & ((1u << kS2BitsB) - 1u);
}

constexpr uint32_t s2_combine(uint32_t a, uint32_t b) {
    return ((a & ((1u << kS2BitsA) - 1u)) << kS2BitsB) |
           (b & ((1u << kS2BitsB) - 1u));
}

constexpr uint32_t s2_read_count(uint32_t value) {
    return ((value >> kS2ShiftA) & ((1u << kS2BitsA) - 1u)) * (1u << kS2BitsB) +
           ((value >> kS2ShiftB) & ((1u << kS2BitsB) - 1u));
}

// bits0–21 原样保留，bits22–31 写入 S2 编码。
constexpr uint32_t s2_write_count(uint32_t value, uint32_t count) {
    return (value & ~(kS2MaskA | kS2MaskB)) |
           (s2_split_a(count) << kS2ShiftA) |
           (s2_split_b(count) << kS2ShiftB);
}

constexpr uint32_t s2_clamp(uint32_t count, bool enabled) {
    const uint32_t limit = max_count(enabled);
    return count > limit ? limit : count;
}

// 开启态使用完整 S2 数量，关闭态只读 b 段并保留 a 段。
constexpr uint32_t effective_read_count(uint32_t value, bool enabled) {
    return enabled ? s2_read_count(value)
                   : ((value >> kS2ShiftB) & ((1u << kS2BitsB) - 1u));
}

// 开启态写入完整 S2，关闭态只写 b 段并保留 a 段与 bits0–21。
constexpr uint32_t effective_write_count(uint32_t value, uint32_t count, bool enabled) {
    return enabled ? s2_write_count(value, count)
                   : (value & ~kS2MaskB) | (s2_split_b(count) << kS2ShiftB);
}

constexpr uint32_t effective_clamp(uint32_t count, bool enabled) {
    return s2_clamp(count, enabled);
}

constexpr uint32_t effective_view_count(uint32_t canonical_count, bool enabled) {
    return enabled ? canonical_count : (canonical_count & ((1u << kS2BitsB) - 1u));
}

constexpr uint32_t field_from_canonical(uint32_t field, uint32_t canonical, bool extended) {
    const uint32_t view = extended ? effective_clamp(canonical, true)
                                   : effective_clamp(canonical & ((1u << kS2BitsB) - 1u), false);
    return effective_write_count(field, view, extended);
}

constexpr uint32_t canonical_from_field(uint32_t field) { return s2_read_count(field); }

constexpr uint32_t view_from_canonical(uint32_t canonical, bool extended) {
    return extended ? effective_clamp(canonical, true)
                    : (canonical & ((1u << kS2BitsB) - 1u));
}

constexpr uint32_t view_limit(bool extended) { return max_count(extended); }

}  // namespace item_count_codec
}  // namespace qol::game
