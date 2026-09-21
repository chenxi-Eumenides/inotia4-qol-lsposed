#pragma once

#include <cstdint>

// 进入存档（读档/新档）加载完成回调。
//
// 语义：仅在「读档 / 新档」加载完成、进入 world 后触发一次；切图返回不触发（切图不经过
// 两个发起点）。发起点用 call_patch 标记 pending，检测任务由统一帧任务管理器注册在
// kFramePointRenderPre（仅 world 态渲染时执行），world 就绪后消费一次。
//
// 线程模型：register/unregister/mark_pending 任意线程可调；回调在游戏主线程执行。
// 本头文件纯逻辑、不依赖 Android，供 host 测试引用。

using SaveEnterFn = void (*)(void* ctx);

// 注册进入存档回调（同一 fn+ctx 幂等）；fn 为空返回 false。
bool save_enter_register(SaveEnterFn fn, void* ctx);

// 注销进入存档回调（移除所有匹配的 fn+ctx）；幂等。
void save_enter_unregister(SaveEnterFn fn, void* ctx);

// 标记「进入存档流程已发起」；任意线程可调。
void save_enter_mark_pending();

// 初始化检测任务（幂等）：向统一帧任务管理器注册 save_enter_tick。
bool save_enter_init();

// 安装两个发起点（读档/新档）的 call_patch；幂等，全部成功或全部回滚。
bool save_enter_host_install_if_ready();

namespace save_enter_detail {

// 触发判定纯函数：pending 且已进入 world 才触发。
constexpr bool should_fire(bool pending, bool in_world) {
    return pending && in_world;
}

}  // namespace save_enter_detail
