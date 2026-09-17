#pragma once

// 命令端口（ARCHITECTURE.md §3.4.3 / §6.4）：外部写入口，命令在主线程执行。
// contracts::QolCommands 的宿主实现；票据状态与取消转发给 infra/command_queue。
//
// 线程纪律：post 可能来自任意线程；drain 只在游戏主线程调用。

#include <cstdint>

#include "contracts/commands.h"

namespace qol::infra {

// 进程级单例端口（函数指针表，静态存储）。
const contracts::QolCommands& commands_port();

// 主线程调用：执行本轮 pending 命令并回收已终态槽位，返回本轮执行条数。
uint32_t commands_drain();

// 测试用：清空槽位表。
void commands_reset_for_test();

}  // namespace qol::infra
