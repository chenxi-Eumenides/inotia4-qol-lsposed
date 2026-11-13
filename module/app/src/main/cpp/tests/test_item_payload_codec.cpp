#include "contracts/item_payload_codec.h"

#include <cstdio>
#include <cstring>

using namespace qol::contracts;

static int g_pass = 0;
static int g_fail = 0;

#define CHECK(condition) do { \
    if (condition) ++g_pass; \
    else { ++g_fail; std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition); } \
} while (0)

static bool headers_equal(const QolItemPayloadHeader& left, const QolItemPayloadHeader& right) {
    return left.uid == right.uid && left.type_flags == right.type_flags &&
           left.count_field == right.count_field && left.magic_rate == right.magic_rate &&
           left.socket == right.socket && left.enchant == right.enchant;
}

int main() {
    const QolItemPayloadHeader header{0x8877665544332211ull, 0xa1b2, 0x12345678,
                                      0x35, 0x64, 0xcdef};
    uint8_t payload[kQolBagTransferPayloadCapacity]{};
    uint32_t payload_size = 0;
    CHECK(item_payload_build(header, nullptr, 0, payload, sizeof(payload), &payload_size));
    CHECK(payload_size == kItemPayloadHeaderSize);
    CHECK(payload[0] == 18);
    QolItemPayloadHeader decoded_header{};
    uint32_t option_count = 0;
    CHECK(item_payload_decode(payload, payload_size, &decoded_header, nullptr, 0, &option_count));
    CHECK(headers_equal(header, decoded_header));
    CHECK(option_count == 0);

    QolItemPayloadOption options[]{{0x0005, -1234}, {0x2007, 321}, {0xa042, -1}};
    CHECK(item_payload_build(header, options, 3, payload, sizeof(payload), &payload_size));
    QolItemPayloadOption decoded_options[3]{};
    CHECK(item_payload_decode(payload, payload_size, &decoded_header, decoded_options, 3,
                              &option_count));
    CHECK(option_count == 3 && headers_equal(header, decoded_header));
    for (uint32_t i = 0; i < 3; ++i) {
        CHECK(decoded_options[i].code == options[i].code);
        CHECK(decoded_options[i].value == options[i].value);
    }
    QolItemPayloadOption max_options[kItemPayloadMaxOptionCount]{};
    CHECK(item_payload_build(header, max_options, kItemPayloadMaxOptionCount, payload,
                             sizeof(payload), &payload_size));
    CHECK(!item_payload_build(header, max_options, kItemPayloadMaxOptionCount + 1, payload,
                              sizeof(payload), &payload_size));

    QolItemPayloadOption invalid_option{0x0080, 1};
    CHECK(!item_payload_build(header, &invalid_option, 1, payload, sizeof(payload), &payload_size));
    CHECK(!item_payload_decode(nullptr, 19, &decoded_header, nullptr, 0, &option_count));
    CHECK(!item_payload_decode(payload, 18, &decoded_header, nullptr, 0, &option_count));
    CHECK(!item_payload_decode(payload, 256, &decoded_header, nullptr, 0, &option_count));
    uint8_t malformed[kQolBagTransferPayloadCapacity]{};
    malformed[0] = 17;
    CHECK(!item_payload_decode(malformed, 19, &decoded_header, nullptr, 0, &option_count));
    malformed[0] = 19;
    CHECK(!item_payload_decode(malformed, 20, &decoded_header, nullptr, 0, &option_count));

    QolItemPayloadOption one_option{0x2007, -42};
    uint8_t source[kQolBagTransferPayloadCapacity]{};
    uint8_t round_trip[kQolBagTransferPayloadCapacity]{};
    uint32_t one_size = 0;
    CHECK(item_payload_build(header, &one_option, 1, source, sizeof(source), &one_size));
    std::memcpy(round_trip, source, one_size);
    round_trip[1] ^= 1;
    CHECK(!item_payload_round_trip_semantically_matches(source, round_trip, one_size));
    std::memcpy(round_trip, source, one_size);
    round_trip[kItemPayloadHeaderSize + 1] |= 0x01;
    CHECK(item_payload_round_trip_semantically_matches(source, round_trip, one_size));
    std::memcpy(round_trip, source, one_size);
    round_trip[kItemPayloadHeaderSize] ^= 1;
    CHECK(!item_payload_round_trip_semantically_matches(source, round_trip, one_size));
    std::memcpy(round_trip, source, one_size);
    round_trip[kItemPayloadHeaderSize + 1] ^= 0x20;
    CHECK(!item_payload_round_trip_semantically_matches(source, round_trip, one_size));
    std::memcpy(round_trip, source, one_size);
    round_trip[kItemPayloadHeaderSize + 2] ^= 1;
    CHECK(!item_payload_round_trip_semantically_matches(source, round_trip, one_size));
    CHECK(!item_payload_round_trip_semantically_matches(source, round_trip, 18));

    CHECK(!item_payload_build(header, nullptr, 0, payload, 18, &payload_size));
    CHECK(g_fail == 0);
    std::printf("item_payload_codec: pass=%d fail=%d\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
