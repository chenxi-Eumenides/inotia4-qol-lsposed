#pragma once

#include <cstdint>

#include "contracts/game_host.h"

// S2 编解码、99/999 策略与有效视图只属于 stack_limit 插件。调用方通过
// QolItemCountApi 访问这些语义，不得依赖本头。

namespace qol::features::stack_limit::impl {

constexpr uint32_t kLegacyMax = 99;
constexpr uint32_t kExtendedMax = 999;

constexpr uint32_t max_count(bool extended) {
    return extended ? kExtendedMax : kLegacyMax;
}

// S2 编码：a 段占 bits22–24，b 段占 bits25–31，数量为 128a+b。
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

// 扩展背包或 999 功能启用时，写侧必须保留完整数量语义。
constexpr bool writeback_needed(bool extension_bag_enabled, bool extended_feature_enabled) {
    return extension_bag_enabled || extended_feature_enabled;
}

// 纯转换不读取游戏对象，也不受 999 原子组门控；驻留后即可提供给消费者。
constexpr uint32_t field_from_canonical(uint32_t field, uint32_t canonical, bool extended) {
    const uint32_t view = extended ? effective_clamp(canonical, true)
                                   : effective_clamp(canonical & ((1u << kS2BitsB) - 1u), false);
    return effective_write_count(field, view, extended);
}

constexpr uint32_t canonical_from_field(uint32_t field) { return s2_read_count(field); }

constexpr uint32_t view_from_canonical(uint32_t canonical, bool extended) {
    return extended ? effective_clamp(canonical, true)
                     : effective_clamp(canonical & ((1u << kS2BitsB) - 1u), false);
}

constexpr uint32_t view_limit(bool extended) { return max_count(extended); }

// 这些原语由 game/access 的过渡 adapter 提供；插件不直接 include game_symbols.h。
contracts::QolItemCountEncoding item_count_encoding(const void* item);
bool read_item_count_field(const void* item, uint32_t* out_value);
bool write_item_count_field(void* item, uint32_t value);

}  // namespace qol::features::stack_limit::impl

namespace qol::host {

void host_item_count_set_provider(const contracts::QolItemCountApi* api);

}  // namespace qol::host
