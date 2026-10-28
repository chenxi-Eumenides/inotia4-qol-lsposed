#include "infra/snapshot_handle.h"

#include <atomic>
#include <cstring>
#include <memory>
#include <new>

namespace qol::infra::snapshot_handle {
namespace {

// 发布缓冲：new uint8_t[size] 不执行零初始化，避免 vector(size) 的额外全量写。
// 读取方通过 Lease 持有 shared_ptr，发布方只替换 atomic 指针，旧缓冲在最后一个
// reader 释放后回收。
struct Buffer {
    explicit Buffer(uint32_t size) : size(size), bytes(new (std::nothrow) uint8_t[size]) {}

    const uint32_t size;
    std::unique_ptr<uint8_t[]> bytes;
};

struct Snapshot {
    explicit Snapshot(uint32_t size) : byte_size(size) {}

    const uint32_t byte_size;
    std::shared_ptr<const Buffer> current;
};

struct Lease {
    explicit Lease(std::shared_ptr<const Buffer> value) : buffer(std::move(value)) {}

    std::shared_ptr<const Buffer> buffer;
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
        // 相同内容不重新分配/拷贝：main menu、not-in-game 等重复发布直接短路。
        std::shared_ptr<const Buffer> current =
            std::atomic_load_explicit(&snapshot->current, std::memory_order_acquire);
        if (current && current->size == byte_size &&
            std::memcmp(current->bytes.get(), data, byte_size) == 0) {
            return true;
        }
        auto next = std::make_shared<Buffer>(byte_size);
        if (!next->bytes) return false;
        std::memcpy(next->bytes.get(), data, byte_size);
        std::shared_ptr<const Buffer> immutable = std::move(next);
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
    std::shared_ptr<const Buffer> current =
        std::atomic_load_explicit(&snapshot->current, std::memory_order_acquire);
    if (!current || !current->bytes) return false;
    try {
        std::unique_ptr<Lease> lease(new Lease(std::move(current)));
        *out_data = lease->buffer->bytes.get();
        *out_size = lease->buffer->size;
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
