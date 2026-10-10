// Host 常驻 item_count facade 契约：三态模式查询、NotReady 不伪装关闭、常驻可用性、
// 空参数语义与 g_base==0 的 fail-closed 行为。S2 关闭态语义回归在 item_count_codec_tests。

#include "host/item_count_facade.h"

#include <cassert>
#include <cstdint>
#include <initializer_list>

#include "contracts/result.h"
#include "contracts/item_count.h"
#include "game/adapter_ports/item_count_facade.h"

// game/access/patches.cpp 链接依赖：本测试不进入编码路径，仅提供符号。
uintptr_t g_base = 0;
uintptr_t fn_resolve(const char*, uintptr_t vma) { return vma; }

namespace {

using qol::contracts::QolItemCountEncoding;
using qol::game::ItemCountExtendedMode;

void assert_pure_conversion_contract() {
    const qol::contracts::QolItemCountApi& api = qol::host::host_item_count_api();
    constexpr uint32_t kMaskA = 0x7u << 22;
    constexpr uint32_t kMaskB = 0x7fu << 25;
    const uint32_t field = 0x00123456u | (5u << 22) | (77u << 25);

    uint32_t canonical = 0;
    assert(api.canonical_from_field(field, &canonical).ok == 0);
    assert(canonical == 5u * 128u + 77u);

    const uint32_t values[] = {99, 100, 127, 128, 199, 999};
    for (uint32_t value : values) {
        for (int32_t extended : {0, 1, -1}) {
            uint32_t converted = 0;
            assert(api.field_from_canonical(field, value, extended, &converted).ok == 0);
            const uint32_t view = extended == 0 ? (value % 128u > 99u ? 99u : value % 128u)
                                                : (value > 999u ? 999u : value);
            const uint32_t expected = extended == 0
                                          ? (field & ~kMaskB) | ((view & 0x7fu) << 25)
                                          : (field & ~(kMaskA | kMaskB)) |
                                                (((view >> 7) & 0x7u) << 22) |
                                                ((view & 0x7fu) << 25);
            assert(converted == expected);
            // 关闭态写只动 b 段，a 段与 bits0–21 原样保留。
            if (extended == 0) assert((converted & ~kMaskB) == (field & ~kMaskB));

            uint32_t round_view = 0;
            assert(api.view_from_canonical(value, extended, &round_view).ok == 0);
            assert(round_view == view);
        }
    }

    uint32_t limit = 0;
    assert(api.view_limit(0, &limit).ok == 0 && limit == 99);
    assert(api.view_limit(1, &limit).ok == 0 && limit == 999);

    uint32_t untouched = 0xabcdef01u;
    assert(api.field_from_canonical(field, 99, 0, nullptr).code == kQolErrInvalidArgument);
    assert(api.canonical_from_field(field, nullptr).code == kQolErrInvalidArgument);
    assert(api.view_from_canonical(99, 0, nullptr).code == kQolErrInvalidArgument);
    assert(api.view_limit(0, nullptr).code == kQolErrInvalidArgument);
    assert(untouched == 0xabcdef01u);
}

void assert_mode_tri_state() {
    const qol::contracts::QolItemCountApi& api = qol::host::host_item_count_api();

    qol::game::item_count_set_extended_mode(ItemCountExtendedMode::kOff);
    uint8_t extended = 9;
    assert(api.read_extended_mode(&extended).ok == 0 && extended == 0);
    assert(!qol::game::item_count_active());

    qol::game::item_count_set_extended_mode(ItemCountExtendedMode::kApplied);
    extended = 0;
    assert(api.read_extended_mode(&extended).ok == 0 && extended == 1);
    assert(qol::game::item_count_active());

    // NotReady = 请求开启但资源组未落地，不伪装关闭。
    qol::game::item_count_set_extended_mode(ItemCountExtendedMode::kNotReady);
    extended = 7;
    assert(api.read_extended_mode(&extended).code == kQolErrNotReady);
    assert(extended == 7);
    assert(!qol::game::item_count_active());

    assert(api.read_extended_mode(nullptr).code == kQolErrInvalidArgument);
}

void assert_resident_and_fail_closed_without_game() {
    const qol::contracts::QolItemCountApi& api = qol::host::host_item_count_api();
    assert(qol::game::item_count_api_if_resident() == &api);
    assert(qol::game::item_count_api_if_resident() != nullptr);

    qol::game::item_count_set_extended_mode(ItemCountExtendedMode::kOff);
    auto encoding = QolItemCountEncoding::kEncoded;
    assert(api.is_encoded(reinterpret_cast<void*>(1), &encoding).ok == 0);
    assert(encoding == QolItemCountEncoding::kUnknown);
    uint32_t count = 41;
    assert(api.canonical(reinterpret_cast<void*>(1), &count).code == kQolErrUnsupported);
    assert(count == 41);
    assert(api.effective(reinterpret_cast<void*>(1), &count).code == kQolErrUnsupported);
    assert(api.write_effective(reinterpret_cast<void*>(1), 1).code == kQolErrUnsupported);
    uint32_t field = 41;
    assert(api.read_encoded_field(reinterpret_cast<void*>(1), &field).code == kQolErrNotReady);
    assert(api.write_encoded_field(reinterpret_cast<void*>(1), 1).code == kQolErrNotReady);

    // 请求开启但未落地：编码相关写读一律 NotReady，不回退成 Unsupported/OK。
    qol::game::item_count_set_extended_mode(ItemCountExtendedMode::kNotReady);
    assert(api.effective(reinterpret_cast<void*>(1), &count).code == kQolErrNotReady);
    int32_t needed = 41;
    assert(api.writeback_needed(reinterpret_cast<void*>(1), 1, &needed).code == kQolErrNotReady);
    assert(needed == 41);
    assert(api.write_effective(reinterpret_cast<void*>(1), 1).code == kQolErrNotReady);

    // 空参数语义与 mode 无关。
    qol::game::item_count_set_extended_mode(ItemCountExtendedMode::kOff);
    assert(api.canonical(nullptr, &count).code == kQolErrInvalidArgument);
    assert(api.canonical(reinterpret_cast<void*>(1), nullptr).code == kQolErrInvalidArgument);
    assert(api.write_effective(nullptr, 1).code == kQolErrInvalidArgument);
    assert(api.writeback_needed(nullptr, 1, &needed).code == kQolErrInvalidArgument);
    assert(api.writeback_needed(reinterpret_cast<void*>(1), 1, nullptr).code ==
           kQolErrInvalidArgument);
    assert(api.is_encoded(nullptr, &encoding).code == kQolErrInvalidArgument);
    assert(api.read_encoded_field(nullptr, &field).code == kQolErrInvalidArgument);
    assert(api.write_encoded_field(nullptr, 1).code == kQolErrInvalidArgument);
}

}  // namespace

int main() {
    assert_pure_conversion_contract();
    assert_mode_tri_state();
    assert_resident_and_fail_closed_without_game();
    qol::game::item_count_set_extended_mode(ItemCountExtendedMode::kOff);
    return 0;
}
