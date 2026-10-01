#pragma once

#include <cstdint>
#include <cstring>
#include <vector>

#include "contracts/host_sdk.h"

namespace test_snapshot_sdk {
namespace contracts = qol::contracts;

struct Buffer {
    explicit Buffer(uint32_t byte_size) : bytes(byte_size) {}
    std::vector<uint8_t> bytes;
};

inline contracts::QolSnapshotHandle create(uint32_t byte_size) {
    return byte_size == 0 ? nullptr : new Buffer(byte_size);
}

inline void destroy(contracts::QolSnapshotHandle handle) {
    delete static_cast<Buffer*>(handle);
}

inline bool publish(contracts::QolSnapshotHandle handle, const void* data, uint32_t byte_size) {
    auto* buffer = static_cast<Buffer*>(handle);
    if (buffer == nullptr || data == nullptr || byte_size != buffer->bytes.size()) return false;
    std::memcpy(buffer->bytes.data(), data, byte_size);
    return true;
}

inline bool acquire(contracts::QolSnapshotHandle handle, const void** out_data,
                   uint32_t* out_size, contracts::QolSnapshotToken* out_token) {
    const auto* buffer = static_cast<const Buffer*>(handle);
    if (buffer == nullptr || out_data == nullptr || out_size == nullptr || out_token == nullptr) {
        return false;
    }
    auto* view = new std::vector<uint8_t>(buffer->bytes);
    *out_data = view->data();
    *out_size = static_cast<uint32_t>(view->size());
    *out_token = view;
    return true;
}

inline void release(contracts::QolSnapshotToken token) {
    delete static_cast<std::vector<uint8_t>*>(token);
}

inline void attach(contracts::QolHostSdk* sdk) {
    sdk->snapshot_create = &create;
    sdk->snapshot_destroy = &destroy;
    sdk->snapshot_publish = &publish;
    sdk->snapshot_acquire = &acquire;
    sdk->snapshot_release = &release;
}

}  // namespace test_snapshot_sdk
