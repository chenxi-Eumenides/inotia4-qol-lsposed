// Host 常驻 item_count facade 契约：三态模式查询、NotReady 不伪装关闭、常驻可用性、
// 空参数语义与 g_base==0 的 fail-closed 行为。S2 关闭态语义回归在 item_count_codec_tests。

#include "host/item_count_facade.h"

#include <cassert>
#include <cstdint>
#include <cstring>
#include <initializer_list>

#include "contracts/result.h"
#include "contracts/item_count.h"
#include "game/adapter_ports/item_count_facade.h"
#include "game/adapter_ports/item_count_codec.h"
#include "game/symbols/game_symbols.h"

// game/access/patches.cpp 链接依赖；行为测试用合成内存提供最小 item 类型表与 I_COUNT。
uintptr_t g_base = 0;
uintptr_t fn_resolve(const char*, uintptr_t vma) { return vma; }

namespace {

using qol::contracts::QolItemCountEncoding;
using qol::game::ItemCountExtendedMode;
namespace codec = qol::game::item_count_codec;

alignas(void*) uint8_t g_fake_base[G_ITEMCLASS_SIZE_GOT_VMA + sizeof(void*)] = {};
uint8_t g_fake_class_data[2 * 7] = {};
uint8_t* g_fake_class_data_ref = g_fake_class_data;
uint8_t g_fake_stride = 7;
alignas(uint32_t) uint8_t g_item[0x14] = {};

uint32_t& item_field() {
    return *reinterpret_cast<uint32_t*>(g_item + I_COUNT);
}

void configure_fake_item(bool encoded, uint32_t field) {
    std::memset(g_fake_base, 0, sizeof(g_fake_base));
    std::memset(g_fake_class_data, 0, sizeof(g_fake_class_data));
    g_fake_class_data[7 + 6] = encoded ? 1u : 0u;
    g_fake_class_data_ref = g_fake_class_data;
    *reinterpret_cast<void**>(g_fake_base + G_ITEMCLASS_DATA_GOT_VMA) =
        &g_fake_class_data_ref;
    *reinterpret_cast<uint8_t**>(g_fake_base + G_ITEMCLASS_SIZE_GOT_VMA) =
        &g_fake_stride;
    g_base = reinterpret_cast<uintptr_t>(g_fake_base);
    std::memset(g_item, 0, sizeof(g_item));
    *reinterpret_cast<uint16_t*>(g_item + I_TYPE) = 1u << 6;
    item_field() = field;
}

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

void assert_state_transition_behavior() {
    const qol::contracts::QolItemCountApi& api = qol::host::host_item_count_api();
    constexpr uint32_t kField = 0x00123456u | (2u << 22) | (72u << 25);
    constexpr uint32_t kOffCanonical = 2u * 128u + 72u;

    configure_fake_item(true, kField);
    const ItemCountExtendedMode modes[] = {ItemCountExtendedMode::kOff,
                                           ItemCountExtendedMode::kNotReady,
                                           ItemCountExtendedMode::kApplied};
    for (ItemCountExtendedMode mode : modes) {
        item_field() = kField;
        qol::game::item_count_set_extended_mode(mode);
        uint8_t extended = 7;
        const QolResult read_mode = api.read_extended_mode(&extended);
        if (mode == ItemCountExtendedMode::kNotReady) {
            assert(read_mode.code == kQolErrNotReady && extended == 7);
            uint32_t effective = 51;
            assert(api.effective(g_item, &effective).code == kQolErrNotReady);
            assert(effective == 51);
            int32_t needed = 41;
            assert(api.writeback_needed(g_item, 99, &needed).code == kQolErrNotReady);
            assert(needed == 41);
            const uint32_t before = item_field();
            assert(api.write_effective(g_item, 99).code == kQolErrNotReady);
            assert(item_field() == before);
            continue;
        }

        const bool applied = mode == ItemCountExtendedMode::kApplied;
        assert(read_mode.ok == 0 && extended == (applied ? 1u : 0u));
        uint32_t effective = 0;
        assert(api.effective(g_item, &effective).ok == 0);
        assert(effective == (applied ? kOffCanonical : 72u));
        int32_t needed = -1;
        assert(api.writeback_needed(g_item, effective, &needed).ok == 0 && needed == 0);
        assert(api.writeback_needed(g_item, 99, &needed).ok == 0 && needed == 1);

        item_field() = kField;
        assert(api.write_effective(g_item, 128).ok == 0);
        if (applied) {
            assert(codec::s2_read_count(item_field()) == 128u);
        } else {
            assert((item_field() & ~codec::kS2MaskB) == (kField & ~codec::kS2MaskB));
            assert(codec::s2_read_count(item_field()) == 2u * 128u + 99u);
        }
    }

    // Non-encoded classes retain the legacy getter value, have no effective write, and never
    // request writeback.
    configure_fake_item(false, kField);
    for (ItemCountExtendedMode mode : {ItemCountExtendedMode::kOff,
                                       ItemCountExtendedMode::kApplied}) {
        qol::game::item_count_set_extended_mode(mode);
        uint32_t count = 0;
        assert(api.canonical(g_item, &count).ok == 0 && count == 1);
        assert(api.effective(g_item, &count).ok == 0 && count == 1);
        assert(api.write_effective(g_item, 1).code == kQolErrUnsupported);
        int32_t needed = 7;
        assert(api.writeback_needed(g_item, 99, &needed).ok == 0 && needed == 0);
    }

    g_base = 0;
    qol::game::item_count_set_extended_mode(ItemCountExtendedMode::kOff);
    uint32_t untouched = 41;
    assert(api.canonical(g_item, &untouched).code == kQolErrUnsupported);
    assert(untouched == 41);
    assert(api.effective(g_item, &untouched).code == kQolErrUnsupported);
    assert(api.write_effective(g_item, 1).code == kQolErrUnsupported);
    int32_t needed = 13;
    assert(api.writeback_needed(g_item, 1, &needed).code == kQolErrUnsupported);
    assert(needed == 13);
}

}  // namespace

int main() {
    assert_pure_conversion_contract();
    assert_mode_tri_state();
    assert_resident_and_fail_closed_without_game();
    assert_state_transition_behavior();
    qol::game::item_count_set_extended_mode(ItemCountExtendedMode::kOff);
    return 0;
}
