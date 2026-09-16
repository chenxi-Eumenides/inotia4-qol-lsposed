// infra/runtime.h 实现：宿主注入帧时钟。infra 不依赖 game，游戏事实由 host 装配时注入。

#include "infra/runtime.h"

#include <atomic>

namespace qol::infra {
namespace {

// 用原子保存函数指针：runtime_init 在装配线程写，frame_count 可能从任意线程读。
std::atomic<int64_t (*)()> g_frame_count{nullptr};

}  // namespace

void runtime_init(const Clock& clock) {
    // 覆盖写入：重复装配以最后一次为准（测试借此注入假时钟）。
    g_frame_count.store(clock.frame_count, std::memory_order_release);
}

int64_t frame_count() {
    int64_t (*fn)() = g_frame_count.load(std::memory_order_acquire);
    return fn != nullptr ? fn() : -1;  // 未注入或注入 nullptr 视为未就绪
}

}  // namespace qol::infra
