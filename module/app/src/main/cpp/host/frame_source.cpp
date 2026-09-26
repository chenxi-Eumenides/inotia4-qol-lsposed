#include "host/frame_source.h"

// 帧源实现（见 host/frame_source.h）。两条锚点作为声明式 kCallPatch 资源安装：
//  1) GAMESTATE_DrawPlay +0x20 的 bl MAP_DrawBase → frame_render_pre_wrapper：
//     先 scheduler_dispatch(kRenderPre) + legacy frame_task_dispatch，
//     再复刻 fn_map_drawbase()。
//  2) MainProcess +0x40 的 bl STATE_NextStartProcess → frame_logic_pre_wrapper：
//     先 scheduler_dispatch(kLogicPre) + legacy frame_task_dispatch，
//     再复刻 fn_state_next_start_process()。
// 两条锚点全成或全退；安装永久生效，不做运行期卸载；幂等。

#include <atomic>
#include <cstdint>
#include <cstring>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "contracts/plugin.h"
#include "contracts/runtime_hooks.h"
#include "infra/capabilities.h"
#include "infra/frame/transition_dispatch.h"
#include "infra/save/save_enter.h"
#include "patch/call_patch.h"
#include "infra/frame/frame_host.h"
#include "infra/frame/frame_task.h"
#include "infra/log/qol_log.h"
#include "game/access/patches.h"
#include "game/access/game_access.h"
#include "hooks/registry.h"
#include "infra/scheduler.h"
#include "patch/instr_patch.h"

namespace qol::host {
namespace {

constexpr const char* kOwner = "core";
constexpr const char* kRenderClaimId = "patch:GAMESTATE_DrawPlay+0x20";
constexpr const char* kLogicClaimId = "patch:MainProcess+0x40";

std::atomic<bool> g_frame_source_installed{false};
std::atomic<bool> g_frame_source_installing{false};

// 替换原 BL MAP_DrawBase：先派发调度器与 legacy 帧任务，再复刻原调用。
void frame_render_pre_wrapper() {
    qol::infra::scheduler_dispatch(qol::infra::FramePoint::kRenderPre);
    frame_task_dispatch(kFramePointRenderPre);
    if (fn_map_drawbase != nullptr) fn_map_drawbase();
}

// 替换原 BL STATE_NextStartProcess：先派发调度器与 legacy 帧任务，再复刻原调用。
// 消费点位于帧计数自增与 Draw 之前。
void frame_logic_pre_wrapper() {
    qol::infra::scheduler_dispatch(qol::infra::FramePoint::kLogicPre);
    frame_task_dispatch(kFramePointLogicPre);
    if (fn_state_next_start_process != nullptr) fn_state_next_start_process();
}

// 两条常驻资源声明（owner="core"，gate=nullptr 表示无开关、常驻）。
const contracts::ResourceClaim kClaims[] = {
    {
        kRenderClaimId,             // id
        "P_GAMESTATE_DRAWPLAY_DRAWBASE",  // symbol：patch 点登记名
        contracts::ResourceMechanism::kCallPatch,
        contracts::ResourcePolicy::kPrivate,
        contracts::ResourceRequirement::kRequired,
        contracts::ResourceFailure::kFailClosed,
        true,                       // resumable
        kOwner,                     // owner
        "渲染开始前帧锚点：DrawPlay 内联 bl 调用点，无函数指针槽可挂",  // reason
        nullptr,                    // gate：常驻
        120,                        // step_budget
        reinterpret_cast<void*>(&frame_render_pre_wrapper),  // handler（BL 目标）
    },
    {
        kLogicClaimId,
        "P_MAINPROCESS_NEXT_STATE",
        contracts::ResourceMechanism::kCallPatch,
        contracts::ResourcePolicy::kPrivate,
        contracts::ResourceRequirement::kRequired,
        contracts::ResourceFailure::kFailClosed,
        true,
        kOwner,
        "逻辑帧开始前帧锚点：MainProcess 内联 bl 调用点，无函数指针槽可挂",
        nullptr,
        120,
        reinterpret_cast<void*>(&frame_logic_pre_wrapper),
    },
};

// 解析站点并物理安装；成功后登记运行期地址（供回滚/诊断）。
bool install_frame_claim(const contracts::ResourceClaim& claim) {
    qol::game::PatchSite site{};
    if (!qol::game::patch_site(claim.symbol, &site)) {
        QOL_LOG_ERROR(QolDomain::kCore, "frame_source: site not found %s", claim.symbol);
        return false;
    }
    const qol::patch::Outcome outcome = qol::patch::install_claim(claim, site.addr);
    if (outcome.ok != 0) {
        QOL_LOG_ERROR(QolDomain::kCore, "frame_source: install failed %s: %s", claim.id,
                      outcome.error != nullptr ? outcome.error : "?");
        return false;
    }
    // 物理安装成功 → 登记表状态置 live（与事务引擎对非入口机制的语义一致；gate=nullptr 常驻）。
    hooks::set_state(claim.id, contracts::ResourceState::kLive);
    hooks::runtime_set(claim.id, site.addr, nullptr);
    return true;
}

}  // namespace

bool frame_source_install() {
    if (g_frame_source_installed.load(std::memory_order_acquire)) return true;
    // exchange 抢占安装权：并发重复调用时后到者直接返回当前状态（幂等）。
    if (g_frame_source_installing.exchange(true, std::memory_order_acq_rel)) {
        return g_frame_source_installed.load(std::memory_order_acquire);
    }

    // patch 原语注入：帧源在 nativeInit 早段、assembly 装配前被调用，须自包含。
    // assembly 亦注入同一组指针（P5 原语迁入 patch/ 后两处一并改指）。
    patch::set_primitives(qol::patch::Primitives{
        &call_patch_install_bl,
        &call_patch_revert_bl,
        &call_patch_install_word,
        &call_patch_revert_word,
    });

    if (!bridge_ready() || g_base == 0 || fn_map_drawbase == nullptr ||
        fn_state_next_start_process == nullptr) {
        g_frame_source_installing.store(false, std::memory_order_release);
        return false;  // 未就绪：不改写字节，允许后续重试
    }

    for (const contracts::ResourceClaim& claim : kClaims) {
        if (!hooks::register_claim(claim)) {
            QOL_LOG_ERROR(QolDomain::kCore, "frame_source: claim registration failed %s", claim.id);
            g_frame_source_installing.store(false, std::memory_order_release);
            return false;
        }
    }

    // 全成或全退：第二条失败时回滚第一条（fail-closed，禁止半装）。
    if (!install_frame_claim(kClaims[0])) {
        g_frame_source_installing.store(false, std::memory_order_release);
        return false;
    }
    if (!install_frame_claim(kClaims[1])) {
        const contracts::ResourceClaim& first = kClaims[0];
        qol::game::PatchSite site{};
        if (qol::game::patch_site(first.symbol, &site)) {
            qol::patch::release_claim(first, site.addr);
            hooks::set_state(first.id, contracts::ResourceState::kFree);
            hooks::runtime_clear(first.id);
        }
        g_frame_source_installing.store(false, std::memory_order_release);
        return false;
    }

    g_frame_source_installed.store(true, std::memory_order_release);
    g_frame_source_installing.store(false, std::memory_order_release);
    QOL_LOG_INFO(QolDomain::kCore, "frame_source installed (render + logic anchors)");
    return true;
}

bool frame_source_installed() {
    return g_frame_source_installed.load(std::memory_order_acquire);
}

}  // namespace qol::host

// 兼容层：transition_dispatch（未迁移消费者）查询逻辑锚点是否安装。
// 旧实现随 infra/frame/frame_host.cpp 删除；安装状态现由 host/frame_source 持有。
bool frame_host_anchors_installed() {
    return qol::host::frame_source_installed();
}

// ---------------------------------------------------------------------------
// T2l-rh：过渡注入面（contracts/runtime_hooks.h）的 host 定义。
// 语义与 infra 原语逐字等价：POD body/ctx 与 infra 的 std::function/std::string 只在本文件
// 包装内部转换；host→infra 允许。表为静态常量（装配前即可用，无「未注入」窗口）。
// ---------------------------------------------------------------------------
namespace qol::contracts {
namespace {

constexpr uint32_t kHostRuntimeBodyCapacity = 4096;

// POD 转换体 → infra std::function 包装：body 写定长缓冲，再转 std::string。
std::string run_transition_body(QolRuntimeTransitionBodyFn fn, void* ctx) {
    char buf[kHostRuntimeBodyCapacity]{};
    uint32_t n = 0;
    fn(ctx, buf, static_cast<uint32_t>(sizeof(buf)), &n);
    const uint32_t size = n <= sizeof(buf) ? n : static_cast<uint32_t>(sizeof(buf));
    return std::string(buf, size);
}

// 输出拷贝：out/out_len 校验 → 容量不足 blocked；返回是否成功拷贝。
QolResult copy_transition_out(const std::string& result, char* out, uint32_t capacity,
                              uint32_t* out_len) {
    if (out == nullptr || out_len == nullptr) {
        return error(kQolErrInvalidArgument, "out and out_len required", 24);
    }
    *out_len = 0;
    if (result.size() > capacity) {
        return error(kQolErrBlocked, "transition output exceeds capacity", 33);
    }
    if (!result.empty()) std::memcpy(out, result.data(), result.size());
    *out_len = static_cast<uint32_t>(result.size());
    return ok();
}

QolResult host_transition_run(QolRuntimeTransitionBodyFn fn, void* ctx, int32_t timeout_ms,
                              char* out, uint32_t capacity, uint32_t* out_len) {
    if (fn == nullptr) return error(kQolErrInvalidArgument, "fn required", 11);
    std::string result;
    const bool executed = transition_run(
        [fn, ctx](std::string* r) { *r = run_transition_body(fn, ctx); }, timeout_ms, &result);
    const QolResult copied = copy_transition_out(result, out, capacity, out_len);
    if (!succeeded(copied)) return copied;
    // false = busy/超时（result 为纯文本）；由调用方决定文案（与迁移前调用点语义一致）。
    return executed ? ok() : error(kQolErrTimeout, out, *out_len);
}

QolResult host_transition_run_inline(QolRuntimeTransitionBodyFn fn, void* ctx, char* out,
                                     uint32_t capacity, uint32_t* out_len) {
    if (fn == nullptr) return error(kQolErrInvalidArgument, "fn required", 11);
    std::string result;
    const bool executed = transition_run_inline(
        [fn, ctx](std::string* r) { *r = run_transition_body(fn, ctx); }, &result);
    const QolResult copied = copy_transition_out(result, out, capacity, out_len);
    if (!succeeded(copied)) return copied;
    return executed ? ok() : error(kQolErrBlocked, out, *out_len);
}

// 帧任务桥：infra 回调只带一个 ctx，用 trampoline + id→ctx 表承载用户 fn/ctx。
// 注销后上下文保留在 retired 列表（不释放），避免「remove 后仍有在途回调」的 UAF；
// 过渡期帧任务数量极小，内存可忽略。
struct HostFrameTaskCtx {
    QolRuntimeFrameTaskFn fn = nullptr;
    void* ctx = nullptr;
};

std::mutex g_host_frame_task_mtx;
std::unordered_map<uint64_t, std::shared_ptr<HostFrameTaskCtx>> g_host_frame_tasks;
std::vector<std::shared_ptr<HostFrameTaskCtx>> g_host_frame_tasks_retired;

bool host_frame_task_trampoline(int64_t frame, void* ctx) {
    auto* slot = static_cast<HostFrameTaskCtx*>(ctx);
    return slot->fn(frame, slot->ctx) != 0;
}

uint64_t host_frame_task_add(int32_t point, QolRuntimeFrameTaskFn fn, void* ctx, int32_t interval,
                             int32_t count) {
    if (fn == nullptr || point < 0 || point >= kFramePointCount) return 0;
    auto slot = std::make_shared<HostFrameTaskCtx>();
    slot->fn = fn;
    slot->ctx = ctx;
    const uint64_t id = frame_task_add(point, &host_frame_task_trampoline, slot.get(), interval,
                                       count);
    if (id == 0) return 0;
    std::lock_guard<std::mutex> lock(g_host_frame_task_mtx);
    g_host_frame_tasks.emplace(id, std::move(slot));
    return id;
}

bool host_frame_task_remove(uint64_t id) {
    const bool removed = frame_task_remove(id);
    std::lock_guard<std::mutex> lock(g_host_frame_task_mtx);
    const auto it = g_host_frame_tasks.find(id);
    if (it != g_host_frame_tasks.end()) {
        g_host_frame_tasks_retired.push_back(std::move(it->second));
        g_host_frame_tasks.erase(it);
    }
    return removed;
}

const void* host_capability_require(const char* id, uint16_t major, uint16_t min_minor) {
    if (id == nullptr) return nullptr;
    return qol::infra::require_capability(id, QolVersionRange{major, min_minor});
}

// W31：contracts 帧点位常量必须与 infra 同值（0=渲染前、1=逻辑前）。
static_assert(kQolRuntimeFramePointRenderPre == kFramePointRenderPre, "frame point drift");
static_assert(kQolRuntimeFramePointLogicPre == kFramePointLogicPre, "frame point drift");

const QolRuntimeHooksApi kQolRuntimeHooksApi{
    kQolRuntimeHooksVersion,    &host_transition_run,
    &host_transition_run_inline,
    &host_frame_task_add,
    &host_frame_task_remove,
    &save_enter_mark_pending,
    &host_capability_require,
};

}  // namespace

const QolRuntimeHooksApi& qol_runtime_hooks() { return kQolRuntimeHooksApi; }

}  // namespace qol::contracts
