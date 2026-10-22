#include "game/adapter_ports/custom_map_resource_port.h"

#include <cstdint>
#include <cstring>
#include <memory>

#include "contracts/game_ports/ports.h"
#include "game/access/game_access.h"
#include "game/access/symbols.h"

namespace qol::game {
namespace {

using LzmaDecodeFn = int32_t (*)(void*, int32_t, void**, int32_t*);

QolResult invalid_argument() {
    return contracts::error(kQolErrInvalidArgument, "invalid map resource input", 26);
}

QolResult not_ready() {
    return contracts::error(kQolErrNotReady, "map resource adapter unavailable", 32);
}

QolResult decode_lzma_container(const uint8_t* container, uint32_t container_size,
                                uint8_t* output, uint32_t output_capacity,
                                uint32_t* out_size) {
    if (out_size == nullptr) return invalid_argument();
    *out_size = 0;
    // The outer marker, LZMA-alone header and at least one compressed byte require 16 bytes.
    if (container == nullptr || output == nullptr || container_size < 16 || output_capacity == 0 ||
        container[0] != 0x01 || container_size - 1 > static_cast<uint32_t>(INT32_MAX)) {
        return invalid_argument();
    }

    const MemFreeFn free_buffer = fn_mem_free;
    if (free_buffer == nullptr) return not_ready();
    const uintptr_t address = resolve_hook_entry("F_LZMA_DECODE_VMA", nullptr);
    if (address == 0) return not_ready();

    void* decoded_buffer = nullptr;
    int32_t decoded_size = 0;
    const auto decode = reinterpret_cast<LzmaDecodeFn>(address);
    const int32_t succeeded = decode(const_cast<uint8_t*>(container + 1),
                                     static_cast<int32_t>(container_size - 1),
                                     &decoded_buffer, &decoded_size);
    const std::unique_ptr<void, MemFreeFn> decoded_guard(decoded_buffer, free_buffer);
    if (succeeded == 0 || decoded_guard == nullptr || decoded_size <= 0) {
        return contracts::error(kQolErrInvalidArgument, "map container decode failed", 27);
    }
    *out_size = static_cast<uint32_t>(decoded_size);
    if (static_cast<uint32_t>(decoded_size) > output_capacity) {
        return contracts::error(kQolErrInvalidArgument, "map output buffer too small", 27);
    }

    std::memcpy(output, decoded_guard.get(), static_cast<size_t>(decoded_size));
    return contracts::ok();
}

QolResult allocate_game_buffer(const uint8_t* data, uint32_t size,
                               void** out_buffer, int32_t* out_size) {
    if (out_buffer != nullptr) *out_buffer = nullptr;
    if (out_size != nullptr) *out_size = 0;
    if (out_buffer == nullptr || out_size == nullptr) return invalid_argument();
    if (data == nullptr || size == 0 || size > static_cast<uint32_t>(INT32_MAX))
        return invalid_argument();

    const MemMallocFn allocate = fn_mem_malloc;
    if (allocate == nullptr) return not_ready();
    void* buffer = allocate(size);
    if (buffer == nullptr)
        return contracts::error(kQolErrInternal, "game resource allocation failed", 31);
    std::memcpy(buffer, data, size);
    *out_buffer = buffer;
    *out_size = static_cast<int32_t>(size);
    return contracts::ok();
}

const contracts::QolCustomMapResourcePort kPort{
    {sizeof(contracts::QolCustomMapResourcePort),
     contracts::kQolPortCustomMapResourceVersion.major,
     contracts::kQolPortCustomMapResourceVersion.minor},
    &decode_lzma_container,
    &allocate_game_buffer,
};

}  // namespace

const contracts::QolCustomMapResourcePort& custom_map_resource_port() { return kPort; }

}  // namespace qol::game
