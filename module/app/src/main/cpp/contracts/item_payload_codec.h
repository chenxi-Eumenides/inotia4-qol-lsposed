#pragma once

#include <cstdint>
#include <cstddef>
#include <cstring>
#include <type_traits>

#include "bag.h"

namespace qol::contracts {

constexpr uint32_t kItemPayloadHeaderSize = 19;
constexpr uint32_t kItemPayloadOptionSize = 4;
// 头字段在 payload 内的固定偏移（[0] 为长度前缀）。所有读写方必须引用这些常量，
// 不得各自复制数字。
constexpr uint32_t kItemPayloadUidOffset = 1;
constexpr uint32_t kItemPayloadTypeFlagsOffset = 9;
constexpr uint32_t kItemPayloadCountFieldOffset = 11;
constexpr uint32_t kItemPayloadMagicRateOffset = 15;
constexpr uint32_t kItemPayloadSocketOffset = 16;
constexpr uint32_t kItemPayloadEnchantOffset = 17;
constexpr uint32_t kItemPayloadMaxSize = kQolBagTransferPayloadMaxSize;
constexpr uint32_t kItemPayloadMaxOptionCount =
    (kItemPayloadMaxSize - kItemPayloadHeaderSize) / kItemPayloadOptionSize;
constexpr uint16_t kItemPayloadOptionSemanticMask = 0xe07f;

struct QolItemPayloadHeader {
    uint64_t uid;
    uint16_t type_flags;
    uint32_t count_field;
    uint8_t magic_rate;
    uint8_t socket;
    uint16_t enchant;
};

struct QolItemPayloadOption {
    uint16_t code;
    int16_t value;
};

static_assert(std::is_standard_layout<QolItemPayloadHeader>::value,
              "QolItemPayloadHeader must remain standard layout");
static_assert(std::is_trivially_copyable<QolItemPayloadHeader>::value,
              "QolItemPayloadHeader must remain trivially copyable");
static_assert(std::is_standard_layout<QolItemPayloadOption>::value,
              "QolItemPayloadOption must remain standard layout");
static_assert(std::is_trivially_copyable<QolItemPayloadOption>::value,
              "QolItemPayloadOption must remain trivially copyable");

inline uint16_t item_payload_read_u16(const uint8_t* bytes) {
    return static_cast<uint16_t>(bytes[0]) |
           static_cast<uint16_t>(static_cast<uint16_t>(bytes[1]) << 8);
}

inline uint32_t item_payload_read_u32(const uint8_t* bytes) {
    return static_cast<uint32_t>(bytes[0]) |
           (static_cast<uint32_t>(bytes[1]) << 8) |
           (static_cast<uint32_t>(bytes[2]) << 16) |
           (static_cast<uint32_t>(bytes[3]) << 24);
}

inline uint64_t item_payload_read_u64(const uint8_t* bytes) {
    return static_cast<uint64_t>(item_payload_read_u32(bytes)) |
           (static_cast<uint64_t>(item_payload_read_u32(bytes + 4)) << 32);
}

inline void item_payload_write_u16(uint8_t* bytes, uint16_t value) {
    bytes[0] = static_cast<uint8_t>(value);
    bytes[1] = static_cast<uint8_t>(value >> 8);
}

inline void item_payload_write_u32(uint8_t* bytes, uint32_t value) {
    for (uint32_t index = 0; index < 4; ++index) {
        bytes[index] = static_cast<uint8_t>(value >> (index * 8));
    }
}

inline void item_payload_write_u64(uint8_t* bytes, uint64_t value) {
    for (uint32_t index = 0; index < 8; ++index) {
        bytes[index] = static_cast<uint8_t>(value >> (index * 8));
    }
}

inline bool item_payload_structurally_valid(const uint8_t* payload, uint32_t size) {
    return payload != nullptr && size >= kItemPayloadHeaderSize && size <= kItemPayloadMaxSize &&
           payload[0] == static_cast<uint8_t>(size - 1) &&
           (size - kItemPayloadHeaderSize) % kItemPayloadOptionSize == 0;
}

inline bool item_payload_decode(const uint8_t* payload, uint32_t size,
                                QolItemPayloadHeader* out_header,
                                QolItemPayloadOption* out_options, uint32_t option_capacity,
                                uint32_t* out_option_count) {
    if (!item_payload_structurally_valid(payload, size) || out_header == nullptr) return false;
    const uint32_t option_count =
        (size - kItemPayloadHeaderSize) / kItemPayloadOptionSize;
    if (option_count != 0 && (out_options == nullptr || option_capacity < option_count)) {
        return false;
    }
    out_header->uid = item_payload_read_u64(payload + kItemPayloadUidOffset);
    out_header->type_flags = item_payload_read_u16(payload + kItemPayloadTypeFlagsOffset);
    out_header->count_field = item_payload_read_u32(payload + kItemPayloadCountFieldOffset);
    out_header->magic_rate = payload[kItemPayloadMagicRateOffset];
    out_header->socket = payload[kItemPayloadSocketOffset];
    out_header->enchant = item_payload_read_u16(payload + kItemPayloadEnchantOffset);
    for (uint32_t index = 0; index < option_count; ++index) {
        const uint8_t* option = payload + kItemPayloadHeaderSize +
                                static_cast<size_t>(index) * kItemPayloadOptionSize;
        out_options[index].code = item_payload_read_u16(option);
        out_options[index].value = static_cast<int16_t>(item_payload_read_u16(option + 2));
    }
    if (out_option_count != nullptr) *out_option_count = option_count;
    return true;
}

inline bool item_payload_build(const QolItemPayloadHeader& header,
                               const QolItemPayloadOption* options, uint32_t option_count,
                               uint8_t* out, uint32_t capacity, uint32_t* out_size) {
    if (option_count > kItemPayloadMaxOptionCount || out == nullptr || out_size == nullptr ||
        (option_count != 0 && options == nullptr)) return false;
    for (uint32_t index = 0; index < option_count; ++index) {
        if ((options[index].code & 0x1f80u) != 0) return false;
    }
    const uint32_t size = kItemPayloadHeaderSize + option_count * kItemPayloadOptionSize;
    if (capacity < size) return false;
    out[0] = static_cast<uint8_t>(size - 1);
    item_payload_write_u64(out + kItemPayloadUidOffset, header.uid);
    item_payload_write_u16(out + kItemPayloadTypeFlagsOffset, header.type_flags);
    item_payload_write_u32(out + kItemPayloadCountFieldOffset, header.count_field);
    out[kItemPayloadMagicRateOffset] = header.magic_rate;
    out[kItemPayloadSocketOffset] = header.socket;
    item_payload_write_u16(out + kItemPayloadEnchantOffset, header.enchant);
    for (uint32_t index = 0; index < option_count; ++index) {
        uint8_t* option = out + kItemPayloadHeaderSize +
                          static_cast<size_t>(index) * kItemPayloadOptionSize;
        item_payload_write_u16(option, options[index].code);
        item_payload_write_u16(option + 2, static_cast<uint16_t>(options[index].value));
    }
    *out_size = size;
    return true;
}

inline bool item_payload_round_trip_semantically_matches(const uint8_t* source,
                                                         const uint8_t* round_trip,
                                                         uint32_t size) {
    if (!item_payload_structurally_valid(source, size) ||
        !item_payload_structurally_valid(round_trip, size)) return false;
    if (std::memcmp(source + 1, round_trip + 1, kItemPayloadHeaderSize - 1) != 0) return false;
    const uint32_t option_count =
        (size - kItemPayloadHeaderSize) / kItemPayloadOptionSize;
    for (uint32_t index = 0; index < option_count; ++index) {
        const uint8_t* source_option = source + kItemPayloadHeaderSize +
                                       static_cast<size_t>(index) * kItemPayloadOptionSize;
        const uint8_t* round_option = round_trip + kItemPayloadHeaderSize +
                                      static_cast<size_t>(index) * kItemPayloadOptionSize;
        if (((item_payload_read_u16(source_option) ^ item_payload_read_u16(round_option)) &
             kItemPayloadOptionSemanticMask) != 0 ||
            std::memcmp(source_option + 2, round_option + 2, 2) != 0) return false;
    }
    return true;
}

}  // namespace qol::contracts
