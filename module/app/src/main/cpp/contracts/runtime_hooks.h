#pragma once

#include <cstdint>
#include <type_traits>

#include "result.h"
#include "version.h"

// 过渡注入面（T2l 授权，v1.0）：game 层域写原语经本表使用 infra 帧/转场/save 机制。
// 语义与 infra 原语逐字等价；表由 host 在装配期静态提供（恒非空），game 层只 include 本头
// （不 include infra/host）。POD 固定宽度、无 STL/异常/RTTI 面，可跨 DSO 传递。
// 本表为迁移期过渡面：各域写原语迁入 game/access 后由本表承接；整目录批清理时随迁移收口。

namespace qol::contracts {

constexpr QolVersion kQolRuntimeHooksVersion{1, 0};

// 帧点位（与 infra frame_task 常量同值；W31 核验）：0=渲染前、1=逻辑前。
constexpr int32_t kQolRuntimeFramePointRenderPre = 0;
constexpr int32_t kQolRuntimeFramePointLogicPre = 1;

// 转场转换体：在游戏主线程相位执行，把 op JSON 等文本写入调用方缓冲。
// 返回 void；out_len 由 body 写为实际字节数（<= capacity）。
typedef void (*QolRuntimeTransitionBodyFn)(void* ctx, char* out, uint32_t capacity,
                                           uint32_t* out_len);

// 帧任务回调：frame = 当前帧号；返回非 0 = 继续，0 = 任务完成自动注销。
typedef int32_t (*QolRuntimeFrameTaskFn)(int64_t frame, void* ctx);

typedef struct QolRuntimeHooksApi {
    QolVersion abi_version;

    // transition_run：fn 在游戏主线程 kLogicPre 相位执行；同步等待；忙/超时 → 非 ok。
    // 成功时 out/out_len 为 fn 产出文本；超时时 out 为 infra 的纯文本 busy/超时文案。
    QolResult (*transition_run)(QolRuntimeTransitionBodyFn fn, void* ctx, int32_t timeout_ms,
                                char* out, uint32_t capacity, uint32_t* out_len);

    // 逻辑相位内联执行（owner 路径；非逻辑相位调用失败）。
    QolResult (*transition_run_inline)(QolRuntimeTransitionBodyFn fn, void* ctx, char* out,
                                       uint32_t capacity, uint32_t* out_len);

    // 帧任务（直通 infra frame_task_add/remove）；point: 0=kRenderPre、1=kLogicPre。
    uint64_t (*frame_task_add)(int32_t point, QolRuntimeFrameTaskFn fn, void* ctx, int32_t interval,
                               int32_t count);
    bool (*frame_task_remove)(uint64_t id);

    // 标记「进入存档流程已发起」（直通 infra/save/save_enter）。
    void (*save_enter_mark_pending)();

    // capability 现取（过渡：game 层无法直接调 infra::require_capability）。
    // 返回 nullptr = 未注册/版本不符；调用方按既有降级语义处理。
    const void* (*capability_require)(const char* id, uint16_t major, uint16_t min_minor);

    // ---- P3c 尾部追加（可空，供 patch/game 层过渡调用 core.save 转换状态机）----

    // 读档/新档发起 thunk：经宿主 save_route 转接 core.save begin_transition
    //（kind: 0=resume 读档、1=begin_new 新档；与 QolSaveTransitionKind 同值）。
    // 准入关闭（prepare_detach 拒绝/槽健康阻断/转换在途）返回 false 且 out_transition=0，
    // 调用方不得继续执行原版转换。out_transition 携带确认序必须复用的 token id。
    bool (*save_transition_begin_thunk)(int32_t kind, int32_t target_slot,
                                        uint64_t* out_transition);

    // 进入存档确认序是否已发布 ready（core.save 确认完成后才允许旧 save_enter 回调与
    // kEventSaveEnter 派发）。false = 确认未完成或失败：调用方不消费 pending、不派发。
    bool (*save_enter_ready)();

    // 取消未决转换：begin 成功后、原版未启动或明确失败的出口显式释放 token/lease
    // （已删 dat 不伪装恢复；provider 撤销 transitioning，session 回 Unavailable）。
    void (*save_transition_cancel_thunk)(uint64_t transition);
} QolRuntimeHooksApi;

static_assert(sizeof(QolVersion) == 4, "QolVersion ABI changed");
static_assert(std::is_standard_layout<QolRuntimeHooksApi>::value,
              "QolRuntimeHooksApi must remain standard layout");
static_assert(std::is_trivially_copyable<QolRuntimeHooksApi>::value,
              "QolRuntimeHooksApi must remain trivially copyable");

// 过渡访问器；定义在 host（host/frame_source.cpp），恒返回非空静态表引用。
const QolRuntimeHooksApi& qol_runtime_hooks();

}  // namespace qol::contracts
