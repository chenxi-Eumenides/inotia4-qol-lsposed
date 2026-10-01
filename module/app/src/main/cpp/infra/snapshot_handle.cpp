#include "infra/snapshot_handle.h"

#include <atomic>
#include <cstring>
#include <memory>
#include <new>
#include <vector>

namespace qol::infra::snapshot_handle {
namespace {

struct Snapshot {
    explicit Snapshot(uint32_t size) : byte_size(size) {}

    const uint32_t byte_size;
    std::shared_ptr<const std::vector<uint8_t>> current;
};

struct Lease {
    explicit Lease(std::shared_ptr<const std::vector<uint8_t>> value)
        : buffer(std::move(value)) {}

    std::shared_ptr<const std::vector<uint8_t>> buffer;
};

Snapshot* as_snapshot(contracts::QolSnapshotHandle handle) {
    return static_cast<Snapshot*>(handle);
}

}  // namespace

contracts::QolSnapshotHandle create(uint32_t byte_size) noexcept {
    if (byte_size == 0) return nullptr;
    try {
        return new Snapshot(byte_size);
    } catch (...) {
        return nullptr;
    }
}

void destroy(contracts::QolSnapshotHandle handle) noexcept {
    delete as_snapshot(handle);
}

bool publish(contracts::QolSnapshotHandle handle, const void* data,
             uint32_t byte_size) noexcept {
    Snapshot* snapshot = as_snapshot(handle);
    if (snapshot == nullptr || data == nullptr || byte_size != snapshot->byte_size) return false;
    try {
        auto next = std::make_shared<std::vector<uint8_t>>(byte_size);
        std::memcpy(next->data(), data, byte_size);
        std::shared_ptr<const std::vector<uint8_t>> immutable = std::move(next);
        std::atomic_store_explicit(&snapshot->current, std::move(immutable),
                                   std::memory_order_release);
        return true;
    } catch (...) {
        return false;
    }
}

bool acquire(contracts::QolSnapshotHandle handle, const void** out_data, uint32_t* out_size,
             contracts::QolSnapshotToken* out_token) noexcept {
    Snapshot* snapshot = as_snapshot(handle);
    if (snapshot == nullptr || out_data == nullptr || out_size == nullptr || out_token == nullptr) {
        return false;
    }
    std::shared_ptr<const std::vector<uint8_t>> current =
        std::atomic_load_explicit(&snapshot->current, std::memory_order_acquire);
    if (!current) return false;
    try {
        std::unique_ptr<Lease> lease(new Lease(std::move(current)));
        const void* view = lease->buffer->data();
        *out_data = view;
        *out_size = snapshot->byte_size;
        *out_token = lease.release();
        return true;
    } catch (...) {
        return false;
    }
}

void release(contracts::QolSnapshotToken token) noexcept {
    delete static_cast<Lease*>(token);
}

}  // namespace qol::infra::snapshot_handle
