#pragma once

#include <algorithm>
#include <cstdint>

#include "contracts/bag.h"

namespace qol::features::core_item::bags {

using BagTransfer = contracts::QolBagTransfer;

inline uint64_t payload_hash(const BagTransfer& transfer) {
    constexpr uint64_t kOffsetBasis = 14695981039346656037ull;
    constexpr uint64_t kPrime = 1099511628211ull;
    const uint32_t size = std::min(transfer.payload_size,
                                   contracts::kQolBagTransferPayloadCapacity);
    uint64_t hash = kOffsetBasis;
    for (uint32_t index = 0; index < size; ++index) {
        hash ^= transfer.payload[index];
        hash *= kPrime;
    }
    return hash;
}

}  // namespace qol::features::core_item::bags
