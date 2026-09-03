#pragma once

#include <string>

// 帧任务调度机制层：通用逐帧任务管理（帧计数驱动，回调注册式）。
// 不含任何具体任务回调（walk/nav 回调在 game_ops_action）。

int frame_task_register(bool (*fn)(void*), void* ctx);
void frame_task_unregister(int id);
void stop_all_tasks();
