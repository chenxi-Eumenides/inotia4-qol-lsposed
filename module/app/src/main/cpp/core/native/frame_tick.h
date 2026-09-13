#pragma once

#include <cstdint>

// 主线程逐帧派发宿主（自动出售阶段 A 基础设施）。
//
// 设计：由主线程 draw-end 等主循环回调每帧调用 frame_tick_dispatch()；同一帧号
// 多次调用只派发一次（按 data_frame_count() 去重）。注册/注销线程安全，回调在
// 调用 frame_tick_dispatch() 的线程（即游戏主线程）执行。
//
// 不依赖任何 feature；帧号来源为 data_frame_count()。

using FrameTickFn = void (*)(void*);

// 注册逐帧回调（同一 fn+ctx 幂等）。fn 为空返回 false。
bool frame_tick_register(FrameTickFn fn, void* ctx);

// 注销逐帧回调；移除所有匹配的 (fn, ctx)。
void frame_tick_unregister(FrameTickFn fn, void* ctx);

// 主线程每帧调用；同一帧号重复调用只派发一次。
void frame_tick_dispatch();

// 当前游戏帧号（封装 data_frame_count()）。
int64_t frame_tick_current_frame();

namespace frame_tick_detail {

// 按帧去重纯函数：帧号有效（>0）且与上次派发帧号不同才派发。
constexpr bool should_dispatch(int64_t frame, int64_t last_frame) {
    return frame > 0 && frame != last_frame;
}

}  // namespace frame_tick_detail
