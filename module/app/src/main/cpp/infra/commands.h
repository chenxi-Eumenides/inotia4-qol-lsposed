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

// 功能生命周期命令（kCommandFeatureLifecycle）的执行入口。宿主装配期注入；
// 未注入时该 kind 的票据置 kFailed（infra 不依赖 host，执行体由宿主提供）。
void commands_set_lifecycle_handler(bool (*handler)(const char* plugin_id, int32_t action));

QolResult commands_register_typed_handler(uint32_t handler_id,
                                          contracts::QolTypedCommandHandler handler, void* ctx);
QolResult commands_unregister_typed_handler(uint32_t handler_id);
QolResult commands_post_typed(uint32_t handler_id, const void* payload, uint32_t size,
                              contracts::QolCommandTicket* out);

// 测试用：清空槽位表。
void commands_reset_for_test();

// 票据状态稳定英文名（pending/done/failed/cancelled；越界 "unknown"），供诊断接口输出。
const char* command_state_name(int32_t state);

}  // namespace qol::infra
