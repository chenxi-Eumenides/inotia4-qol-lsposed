#pragma once

#include <cstdint>
#include <type_traits>

#include "../host_sdk.h"
#include "../result.h"

namespace qol::contracts {

struct QolCustomMapResourcePort {
    QolAbiHeader abi;

    // Decode one complete custom-map LZMA container into caller-owned memory. The adapter
    // releases the game's temporary decode buffer before returning. On insufficient capacity,
    // out_size reports the required decoded byte count while output remains untouched.
    QolResult (*decode_lzma_container)(const uint8_t* container, uint32_t container_size,
                                       uint8_t* output, uint32_t output_capacity,
                                       uint32_t* out_size);

    // Copy bytes into the game resource heap. On success ownership of out_buffer transfers to
    // the hooked game caller, which releases it with the matching game allocator.
    QolResult (*allocate_game_buffer)(const uint8_t* data, uint32_t size,
                                      void** out_buffer, int32_t* out_size);
};

static_assert(std::is_standard_layout<QolCustomMapResourcePort>::value,
              "QolCustomMapResourcePort must remain standard layout");
static_assert(std::is_trivially_copyable<QolCustomMapResourcePort>::value,
              "QolCustomMapResourcePort must remain trivially copyable");

}  // namespace qol::contracts
