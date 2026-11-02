#pragma once

// 命令端口契约（ARCHITECTURE.md §3.4.3）。
// 命令为 POD；post 在主线程执行，结果写回票据；取消语义由命令声明。

#include <cstdint>

#include "result.h"

namespace qol::contracts {

using QolCommandTicket = uint64_t; // 0 = 无效票据
constexpr uint32_t kTypedCommandPayloadCapacity = 128;

// 票据状态取值域（与 infra::CommandState 一致）。unknown=票据不存在；expired=状态记录已被
// 有界窗口淘汰——两者与 failed（命令执行失败）语义不同，调用方必须区分。
enum : int32_t {
    kCommandStatePending = 0,
    kCommandStateDone = 1,
    kCommandStateFailed = 2,
    kCommandStateCancelled = 3,
    kCommandStateUnknown = 4,
    kCommandStateExpired = 5,
};

enum : uint32_t {
    kCommandSetConfigBool = 1,
    kCommandFeatureLifecycle = 2,
};

// 生命周期命令动作（kind == kCommandFeatureLifecycle 时有效）；key = 插件 id。
enum : int32_t {
    kFeatureLifecycleEnable = 1,
    kFeatureLifecycleDisable = 2,
    kFeatureLifecycleDeactivate = 3,
    kFeatureLifecycleActivate = 4,
};

struct QolCommand {
    uint32_t kind;
    const char* key;
    int32_t int_value;
};

struct QolTypedCommandHeader {
    uint32_t handler_id;
    uint32_t payload_bytes;
};

using QolTypedCommandHandler = bool (*)(void* ctx, const uint8_t* payload, uint32_t size);

struct QolCommands {
    QolResult (*post)(QolCommandTicket* out, const QolCommand* cmd); // 主线程执行
    int32_t (*state)(QolCommandTicket ticket);                       // pending/done/failed/cancelled
    QolResult (*cancel)(QolCommandTicket ticket);
    QolResult (*register_typed_handler)(uint32_t handler_id,
                                        QolTypedCommandHandler handler, void* ctx);
    QolResult (*unregister_typed_handler)(uint32_t handler_id);
    QolResult (*post_typed)(uint32_t handler_id, const void* payload, uint32_t size,
                            QolCommandTicket* out);
};

// 可写结果的 typed 命令处理器：执行命令并把该 API 迁移前的结果 JSON 写入 out（capacity 由宿主
// 提供，out_size 为实际长度）。与 QolTypedCommandHandler 同 id 空间，但允许回取结果。
// 保留旧 QolTypedCommandHandler 签名不变；本类型是其“可写结果”的追加版本。
using QolTypedResultCommandHandler = bool (*)(void* ctx, const uint8_t* payload, uint32_t size,
                                              char* out, uint32_t capacity, uint32_t* out_size);

// 票据结果回取面（QolHostSdk 尾部追加，按 abi.size 协商；老插件不消费）。
struct QolTicketResultApi {
    // 注册/注销可写结果的 typed 处理器；与 register_typed_handler 同 id 空间，同 id 幂等。
    QolResult (*register_typed_result_handler)(uint32_t handler_id,
                                               QolTypedResultCommandHandler handler, void* ctx);
    QolResult (*unregister_typed_result_handler)(uint32_t handler_id);
    // 读取票据终态结果：成功表示 out 已写入结果；kQolErrNotFound 表示签名/票据未知/无结果/已淘汰。
    QolResult (*read_result)(QolCommandTicket ticket, char* out, uint32_t capacity,
                             uint32_t* out_size);
    // 细化票据状态（含 unknown/expired）；返回值即状态码。
    int32_t (*ticket_state)(QolCommandTicket ticket);
    // 延迟结算（阶段 5 save 写面）：结果处理器判定尚未就绪时请求「保持 pending、下一帧重试」；
    // 仅在命令执行期间有效，受宿主延迟上限约束（超限以结算超时失败，不静默成功）。
    QolResult (*defer_current_result)(void);
    // 当前命令执行次数（首次 0，延迟重试逐次递增）：供处理器区分「首次执行」与「等待就绪」。
    uint32_t (*current_attempt)(void);
};

}  // namespace qol::contracts
