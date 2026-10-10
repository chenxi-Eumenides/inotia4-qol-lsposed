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

// Host-private projection path: admits config commands that public QolCommands.post rejects.
QolResult commands_post_config_bool(const char* key, bool value,
                                    contracts::QolCommandTicket* out_ticket);

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

// 可写结果的 typed 处理器：同 id 空间，与 commands_register_typed_handler 互斥；执行时把该 API
// 迁移前的结果 JSON 写入票据缓冲，供 commands_read_result 在终态回取（明确尽力语义）。
QolResult commands_register_typed_result_handler(uint32_t handler_id,
                                                 contracts::QolTypedResultCommandHandler handler,
                                                 void* ctx);
QolResult commands_unregister_typed_result_handler(uint32_t handler_id);

// 回取票据终态结果：成功（out_size=实际长度）；否则 kQolErrNotFound（无结果/未知/过期/容量不足）。
QolResult commands_read_result(contracts::QolCommandTicket ticket, char* out, uint32_t capacity,
                               uint32_t* out_size);

// 票据状态（含 unknown/expired），返回 contracts 状态码。
int32_t commands_ticket_state(contracts::QolCommandTicket ticket);

// 延迟结算：结果处理器判定尚未就绪时请求「保持 pending，下一帧重试」（仅在命令执行期间有效，
// 受 Command.defer_limit_frames 约束）；返回当前命令的执行次数（首次 0，逐次递增）。
QolResult commands_defer_current_result();
uint32_t commands_current_attempt();

// 测试用：清空槽位表。
void commands_reset_for_test();

// 票据状态稳定英文名（pending/done/failed/cancelled；越界 "unknown"），供诊断接口输出。
const char* command_state_name(int32_t state);

}  // namespace qol::infra
