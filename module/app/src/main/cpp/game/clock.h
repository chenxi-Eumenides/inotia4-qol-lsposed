#pragma once

// 游戏层最小面（ARCHITECTURE.md §5）：帧计数是游戏事实，由 game/ 提供。
// host 装配时把它注入 infra（见 infra/runtime.h），infra 因此不依赖 game。

#include <cstdint>

namespace qol::game {

// 当前游戏帧号；基址/符号未就绪返回 -1。可从任意线程调用，无副作用。
int64_t frame_count();

}  // namespace qol::game
