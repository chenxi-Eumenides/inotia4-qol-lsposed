#pragma once

#include <cstdint>

namespace stack_codec {

enum class CountEncoding : uint8_t {
    kUnknown,
    kNotEncoded,
    kEncoded,
};

constexpr uint32_t kLegacyMax = 99;
constexpr uint32_t kExtendedMax = 999;

constexpr uint32_t max_count(bool extended) {
    return extended ? kExtendedMax : kLegacyMax;
}

// 原生袋对象 +0x10 的 bit0..24 是容量，bit25..31 才是对象 marker。
// 该字段禁止使用 write_count：普通物品数量位从 bit22 起，会覆盖袋容量。
constexpr uint32_t kNativeBagObjectMarkerShift = 25;
constexpr uint32_t kNativeBagObjectMarkerMask = 0x7Fu << kNativeBagObjectMarkerShift;

constexpr uint32_t write_native_bag_object_marker(uint32_t value) {
    return (value & ~kNativeBagObjectMarkerMask) |
           (1u << kNativeBagObjectMarkerShift);
}

// ---------------------------------------------------------------------------
// S2 编码（拆段 10-bit count：a 段 bits22–24 + b 段 bits25–31）
// ---------------------------------------------------------------------------
// 布局语义：数量 count(0..1023) 拆为两段写入字段：
//   a = count >> 7   （3 位，bits22–24）
//   b = count & 0x7F （7 位，bits25–31）
//   count = a * 128 + b
// 数量位读写只有这一种布局：所有读路径必须经 s2_read_count/effective_read_count、
// 所有写路径必须经 s2_write_count/effective_write_count。s2_* 本身与任何运行时
// 配置无关；模式（堆叠上限开关）只允许经下方 effective_* 模式感知 API 进入读写。
//
// 类别门控（强制）：s2_* 写 API 仅允许对 count-encoded 类别调用。以下字段布局
// 与 S2 位区重叠，绝对不得经 s2_write_count 写入：
//   - 宝石类别：属性位在 bits18–23，与 a 段（bits22–24）部分重叠；
//   - 袋容量（native bag object +0x10）：bit0..24 为容量，覆盖 a 段全部；
//   - 装备对象 marker：bit25..31 为 marker（见 write_native_bag_object_marker），
//     覆盖 b 段全部。
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

// 清除 bits22–31 后写入 S2 编码；bits0–21 原样保留。
constexpr uint32_t s2_write_count(uint32_t value, uint32_t count) {
    return (value & ~(kS2MaskA | kS2MaskB)) |
           (s2_split_a(count) << kS2ShiftA) |
           (s2_split_b(count) << kS2ShiftB);
}

// 操作层业务上限：开扩展 999 / 关扩展 99（复用上限常量）。
// 模式仅影响此 clamp（写路径的业务上限），绝不参与解码或编码。
constexpr uint32_t s2_clamp(uint32_t count, bool enabled) {
    const uint32_t limit = max_count(enabled);
    return count > limit ? limit : count;
}

// ---------------------------------------------------------------------------
// 模式感知读写（R-47 决策 b）：堆叠上限启用态 = S2 全量 `128a+b`（上限 999）；
// 关闭态 = 低 7 位视图 b（上限 99），a（bits22–24）不读、不写、不参与运算，
// 只原样保留。因此关闭期的写不会破坏 a，重新启用后经 s2_read_count 可读回
// 完整 canonical 值（例：canonical 199=a1+b71 → 关闭态读写作 71；关闭态写 99
// 只改 b → 重开读 128+99=227）。
// ---------------------------------------------------------------------------

// 模式感知读：启用态返回 S2 全量 128a+b；关闭态只读 b 段（a 不读）。
constexpr uint32_t effective_read_count(uint32_t value, bool enabled) {
    return enabled ? s2_read_count(value)
                   : ((value >> kS2ShiftB) & ((1u << kS2BitsB) - 1u));
}

// 模式感知写：启用态 s2_write_count 全量拆段（进位内建）；关闭态只写 b 段，
// a（bits22–24）与 bits0–21 原样保留。count 入参须处于当前模式视图域
//（启用 0..999 / 关闭 0..99，调用方先经 effective_clamp 收敛）。
constexpr uint32_t effective_write_count(uint32_t value, uint32_t count, bool enabled) {
    return enabled ? s2_write_count(value, count)
                   : (value & ~kS2MaskB) | (s2_split_b(count) << kS2ShiftB);
}

// 模式感知 clamp：启用 999 / 关闭 99（与 s2_clamp 同口径，命名对齐模式感知读写）。
constexpr uint32_t effective_clamp(uint32_t count, bool enabled) {
    return s2_clamp(count, enabled);
}

// 已解码 canonical 总量（descriptor int 域，非位域）的模式视图：
// 启用态原样返回 canonical；关闭态返回 b 视图 = canonical mod 128（a 不读）。
constexpr uint32_t effective_view_count(uint32_t canonical_count, bool enabled) {
    return enabled ? canonical_count : (canonical_count & ((1u << kS2BitsB) - 1u));
}

// S2 写侧回写总门控：堆叠上限开启即需要 S2 全量写侧进位——必须与读侧 getter 门控
// `effective_read_count(..., stack_limit_enabled())` 对齐，否则会出现「读按 S2 全量、
// 写只落 b 段」的不对称（99+99 被截断为 70）。
// 扩展背包开启时保持既有行为：关闭态下 effective_write_count 只写 b 段，回写幂等。
constexpr bool writeback_needed(bool extension_bag_enabled, bool stack_limit_enabled) {
    return extension_bag_enabled || stack_limit_enabled;
}

}  // namespace stack_codec
