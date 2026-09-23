#pragma once

#include <atomic>
#include <cstdint>
#include <type_traits>

namespace qol::features::system_facts {

// system_facts 的领域快照只允许由主线程发布，HTTP worker 线程读取；插件不能依赖
// infra 私有实现，因此在自身目录保留相同的 seqlock 语义。
template <typename T>
class Snapshot {
    static_assert(std::is_trivially_copyable<T>::value,
                  "system_facts Snapshot<T> requires a trivially copyable type");

public:
    Snapshot() = default;
    Snapshot(const Snapshot&) = delete;
    Snapshot& operator=(const Snapshot&) = delete;

    void publish(const T& value) {
        const uint32_t sequence = sequence_.load(std::memory_order_relaxed);
        sequence_.store(sequence + 1, std::memory_order_relaxed);
        std::atomic_thread_fence(std::memory_order_release);
        data_ = value;
        std::atomic_thread_fence(std::memory_order_release);
        sequence_.store(sequence + 2, std::memory_order_relaxed);
    }

    bool read(T& out) const {
        for (int attempt = 0; attempt < 8; ++attempt) {
            const uint32_t before = sequence_.load(std::memory_order_acquire);
            if (before < 2u || (before & 1u) != 0u) continue;
            out = data_;
            std::atomic_thread_fence(std::memory_order_acquire);
            if (before == sequence_.load(std::memory_order_acquire)) return true;
        }
        return false;
    }

private:
    T data_{};
    std::atomic<uint32_t> sequence_{0};
};

}  // namespace qol::features::system_facts
