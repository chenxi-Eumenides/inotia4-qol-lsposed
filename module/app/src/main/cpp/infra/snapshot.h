#pragma once

// 只读快照（ARCHITECTURE.md §6.2）：仅可平凡复制类型；单写者（主线程）发布，
// 任意线程读取。seqlock 语义：写者发布期间读者重试或返回失败，绝不返回撕裂数据。
//
// 约束：T 不得包含函数指针、迭代器或指向游戏内存的裸指针。

#include <atomic>
#include <cstdint>
#include <type_traits>

namespace qol::infra {

template <typename T>
class Snapshot {
    static_assert(std::is_trivially_copyable<T>::value,
                  "Snapshot<T> 仅支持可平凡复制类型（POD）");

public:
    Snapshot() = default;
    Snapshot(const Snapshot&) = delete;
    Snapshot& operator=(const Snapshot&) = delete;

    // 单写者（游戏主线程）调用。
    void publish(const T& value) {
        const uint32_t seq = seq_.load(std::memory_order_relaxed);
        seq_.store(seq + 1, std::memory_order_relaxed);  // 奇数：写入中
        std::atomic_thread_fence(std::memory_order_release);
        data_ = value;
        std::atomic_thread_fence(std::memory_order_release);
        seq_.store(seq + 2, std::memory_order_relaxed);  // 偶数：稳定
    }

    // 任意线程调用；成功返回 true 并填充 out，写入竞争期内重试失败返回 false。
    bool read(T& out) const {
        for (int attempt = 0; attempt < 8; ++attempt) {
            const uint32_t before = seq_.load(std::memory_order_acquire);
            if (before < 2u) return false;  // 尚未发布过任何快照
            if ((before & 1u) != 0u) continue;
            out = data_;
            std::atomic_thread_fence(std::memory_order_acquire);
            const uint32_t after = seq_.load(std::memory_order_acquire);
            if (before == after) return true;
        }
        return false;
    }

    // 是否已发布过至少一次且当前不处于写入中。
    bool valid() const {
        const uint32_t seq = seq_.load(std::memory_order_acquire);
        return seq >= 2u && (seq & 1u) == 0u;
    }

    uint32_t sequence() const { return seq_.load(std::memory_order_acquire); }

private:
    T data_{};
    std::atomic<uint32_t> seq_{0};
};

}  // namespace qol::infra
