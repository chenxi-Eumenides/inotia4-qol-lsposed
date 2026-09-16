#pragma once

// 运行时基础设施的宿主注入接口（ARCHITECTURE.md §2.2）：infra 不依赖 game，
// 帧时钟等游戏事实由 host 在装配时注入。

#include <cstdint>

namespace qol::infra {

struct Clock {
    // 返回当前游戏帧号；未就绪返回 -1。实现必须可从任意线程调用且无副作用。
    int64_t (*frame_count)();
};

// 装配期调用一次；重复调用以最后一次为准（用于测试可注入假时钟）。
void runtime_init(const Clock& clock);

// 未注入或未就绪返回 -1。
int64_t frame_count();

}  // namespace qol::infra
