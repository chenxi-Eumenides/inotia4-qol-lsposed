// Game Adapter 私有 S2 codec 纯函数契约：a/b 段拆分/合并、关闭态只读/写 b 段并保留 a 段、
// canonical 与 field 的显式 extended 转换、clamp/effective 位转换。

#include "game/adapter_ports/item_count_codec.h"

#include <cassert>
#include <cstdint>
#include <initializer_list>

namespace codec = qol::game::item_count_codec;

namespace {

constexpr uint32_t kMaskA = ((1u << codec::kS2BitsA) - 1u) << codec::kS2ShiftA;
constexpr uint32_t kMaskB = ((1u << codec::kS2BitsB) - 1u) << codec::kS2ShiftB;

void assert_split_combine() {
    for (uint32_t count = 0; count <= codec::kS2Max; ++count) {
        const uint32_t a = codec::s2_split_a(count);
        const uint32_t b = codec::s2_split_b(count);
        assert(a < (1u << codec::kS2BitsA));
        assert(b < (1u << codec::kS2BitsB));
        assert(codec::s2_combine(a, b) == count);
    }
    assert(codec::s2_combine(0, 0) == 0);
    assert(codec::s2_combine(7, 127) == codec::kS2Max);
}

void assert_write_read_roundtrip() {
    uint32_t field = 0x00123456u;
    for (uint32_t count : {0u, 1u, 99u, 100u, 127u, 128u, 999u, 1023u}) {
        const uint32_t written = codec::s2_write_count(field, count);
        assert(codec::s2_read_count(written) == count);
        // bits0–21 原样保留。
        assert((written & ~(kMaskA | kMaskB)) == (field & ~(kMaskA | kMaskB)));
    }
}

void assert_closed_state_semantics() {
    const uint32_t field = 0x00123456u | (5u << codec::kS2ShiftA) | (77u << codec::kS2ShiftB);

    // 关闭态读只取 b 段并保留 a 段；开启态读完整 128a+b。
    assert(codec::effective_read_count(field, false) == 77u);
    assert(codec::effective_read_count(field, true) == 5u * 128u + 77u);

    // 关闭态写只动 b 段，a 段与 bits0–21 不变。
    const uint32_t closed = codec::effective_write_count(field, 42u, false);
    assert((closed & ~kMaskB) == (field & ~kMaskB));
    assert(codec::s2_read_count(closed) == 5u * 128u + 42u);

    // 开启态写回完整 S2。
    const uint32_t opened = codec::effective_write_count(field, 300u, true);
    assert(codec::s2_read_count(opened) == 300u);

    // 读视图关闭态取原始 b 段；写上限仍为 99，启用态视图上限为 999。
    assert(codec::effective_view_count(5u * 128u + 77u, false) == 77u);
    assert(codec::effective_view_count(5u * 128u + 77u, true) == 5u * 128u + 77u);
    assert(codec::view_from_canonical(107u, false) == 107u);
    assert(codec::view_from_canonical(206u, false) == 78u);
    assert(codec::effective_clamp(200u, false) == 99u);
    assert(codec::effective_clamp(200u, true) == 200u);
    assert(codec::effective_clamp(2000u, true) == 999u);
}

void assert_field_and_view_conversions() {
    const uint32_t field = 0x00123456u | (5u << codec::kS2ShiftA) | (77u << codec::kS2ShiftB);
    assert(codec::canonical_from_field(field) == 5u * 128u + 77u);

    const uint32_t canonical_values[] = {99, 100, 127, 128, 199, 999};
    for (uint32_t value : canonical_values) {
        for (bool extended : {false, true}) {
            const uint32_t converted = codec::field_from_canonical(field, value, extended);
            const uint32_t write_view = extended ? (value > 999u ? 999u : value)
                                                 : (value % 128u > 99u ? 99u : value % 128u);
            const uint32_t expected = extended
                                           ? (field & ~(kMaskA | kMaskB)) |
                                                 (((write_view >> 7) & 0x7u) << codec::kS2ShiftA) |
                                                 ((write_view & 0x7fu) << codec::kS2ShiftB)
                                           : (field & ~kMaskB) |
                                                 ((write_view & 0x7fu) << codec::kS2ShiftB);
            assert(converted == expected);
            if (!extended) assert((converted & ~kMaskB) == (field & ~kMaskB));

            const uint32_t read_view = extended ? (value > 999u ? 999u : value)
                                                : (value & 0x7fu);
            assert(codec::view_from_canonical(value, extended) == read_view);
        }
    }

    assert(codec::view_limit(false) == 99u);
    assert(codec::view_limit(true) == 999u);
    assert(codec::max_count(false) == 99u);
    assert(codec::max_count(true) == 999u);
}

void assert_boundary_matrix() {
    constexpr uint32_t kBits0To21 = (1u << 22) - 1u;
    const uint32_t field = 0x00234567u | (3u << codec::kS2ShiftA) | (44u << codec::kS2ShiftB);
    const uint32_t canonical_values[] = {99, 100, 127, 128, 199, 999, 1000, 1023};

    for (bool extended : {false, true}) {
        assert(codec::view_limit(extended) == (extended ? 999u : 99u));
        for (uint32_t value : canonical_values) {
            const uint32_t value_field = codec::s2_write_count(field, value);
            assert(codec::effective_read_count(value_field, extended) ==
                   (extended ? value : codec::s2_split_b(value)));

            const uint32_t effective_write = codec::effective_write_count(field, value, extended);
            assert((effective_write & kBits0To21) == (field & kBits0To21));
            if (!extended) {
                assert((effective_write & ~kMaskB) == (field & ~kMaskB));
                assert(codec::effective_read_count(effective_write, false) == codec::s2_split_b(value));
            } else {
                assert(codec::effective_read_count(effective_write, true) == (value & 1023u));
            }

            const uint32_t clamped = codec::s2_clamp(value, extended);
            assert(clamped == (value > (extended ? 999u : 99u)
                                   ? (extended ? 999u : 99u)
                                   : value));
            const uint32_t expected_view = extended ? codec::s2_clamp(value, true)
                                                    : codec::s2_split_b(value);
            const uint32_t write_view = extended
                                            ? codec::s2_clamp(value, true)
                                            : codec::s2_clamp(codec::s2_split_b(value), false);
            const uint32_t converted = codec::field_from_canonical(field, value, extended);
            assert((converted & kBits0To21) == (field & kBits0To21));
            if (!extended) assert((converted & ~kMaskB) == (field & ~kMaskB));
            assert(codec::canonical_from_field(converted) ==
                   (extended ? write_view
                             : codec::s2_combine(codec::s2_split_a(codec::s2_read_count(field)),
                                                 write_view)));
            assert(codec::view_from_canonical(value, extended) == expected_view);
        }
    }
}

}  // namespace

int main() {
    assert_split_combine();
    assert_write_read_roundtrip();
    assert_closed_state_semantics();
    assert_field_and_view_conversions();
    assert_boundary_matrix();
    return 0;
}
