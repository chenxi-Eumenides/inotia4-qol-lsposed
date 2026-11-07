#include "save_transition.h"

#include <cstring>
#include <limits>
#include <map>
#include <mutex>
#include <vector>

#include "contracts/slot_operation.h"
#include "plugins/core.save/log.h"
#include "save_journal.h"
#include "slot_data_provider.h"

namespace qol::features::core_save {

namespace {
void (*g_state_publisher)(bool ready, int32_t slot) = nullptr;
}  // namespace

void save_transition_set_state_publisher(void (*publisher)(bool ready, int32_t slot)) {
    g_state_publisher = publisher;
}

void publish_save_ready(bool ready, int32_t slot) {
    if (g_state_publisher != nullptr) g_state_publisher(ready, slot);
}
namespace {
using namespace qol::contracts;
constexpr auto value = [](auto v) { return static_cast<uint32_t>(v); };
QolResult fail(int32_t code, const char* text) {
    return error(code, text, static_cast<uint32_t>(std::strlen(text)));
}
// QolResult.message 可为 nullptr（contracts/result.h）：日志只做非空保护，不改返回值。
const char* log_message_of(const QolResult& result) {
    return result.message != nullptr ? result.message : "";
}

struct PendingTransition {
    uint64_t id;
    TransitionKind kind;
    int32_t target_slot;
    uint64_t old_epoch;
    TransitionPhase phase;
    QolSlotOperationLease lease{};
    bool lease_held = false;
};

struct Runtime {
    std::mutex mutex;
    QolOwnerToken owner = 0;
    QolOwnerVerifyTokenFn verify = nullptr;
    const QolOwnerStoreApi* backend = nullptr;
    const QolSlotOperationApi* operation = nullptr;
    int32_t (*is_main_thread)() = nullptr;
    bool (*transition_allowed)(int32_t, void*) = nullptr;
    void* gate_ctx = nullptr;
    QolResult (*journal_verdict)(int32_t, const QolSlotOperationLease*, void*) = nullptr;
    void* journal_ctx = nullptr;
    uint64_t next_token = 1;
    PendingTransition pending{};  // 同一时刻至多一个未决转换（provider transitioning 互斥）。
    bool has_pending = false;
    bool ready_published = false;
    std::map<uint64_t, TransitionResult> terminal;
    std::vector<uint64_t> terminal_order;
};
Runtime g;

void record_terminal(uint64_t token, const TransitionResult& result) {
    constexpr std::size_t kTerminalWindow = 256;
    g.terminal[token] = result;
    g.terminal_order.push_back(token);
    if (g.terminal_order.size() > kTerminalWindow) {
        g.terminal.erase(g.terminal_order.front());
        g.terminal_order.erase(g.terminal_order.begin());
    }
}

bool main_thread() {
    int32_t (*is_main)() = nullptr;
    {
        std::lock_guard<std::mutex> lock(g.mutex);
        is_main = g.is_main_thread;
    }
    return is_main != nullptr && is_main() != 0;
}

void release_lease(PendingTransition& pending) {
    if (!pending.lease_held) return;
    if (g.operation != nullptr)
        (void)g.operation->release(g.operation->host_ctx, g.owner, &pending.lease);
    pending.lease_held = false;
}

// 未决 journal 的严格分派：无注入时任何可解码/无法解码的 journal 都阻断确认序，
// 不自动提升、不重放、不删除证据；注入的协调器分派决定放行或阻断。
QolResult journal_check(int32_t slot, const QolSlotOperationLease* lease) {
    if (g.backend == nullptr) return ok();
    std::vector<uint8_t> bytes(1024 * 1024);
    uint64_t size = 0;
    uint16_t version = 0;
    const auto read = g.backend->read_section(g.owner, slot, kSaveJournalSection, bytes.data(),
                                              bytes.size(), &size, &version);
    if (read.code == kQolErrNotFound) return ok();  // 无未决 journal。
    if (!succeeded(read)) {                         // 不可读≠缺失：阻断确认序。
        static CoreSaveLogThrottle throttle;
        if (throttle.due(5000))
            CS_LOG_WARN(kCoreSaveLogDomain, "journal read failed slot=%d code=%d", slot, read.code);
        return read;
    }
    SaveJournal journal;
    const auto decoded = decode_save_journal(version, slot, bytes.data(), size, &journal);
    if (decoded == JournalDecode::kValid && journal.stage == JournalStage::kOriginalConfirmed &&
        g.journal_verdict != nullptr)
        return g.journal_verdict(slot, lease, g.journal_ctx);
    if (decoded == JournalDecode::kValid) {
        if (journal.stage == JournalStage::kPrepared) {
            static CoreSaveLogThrottle throttle;
            if (throttle.due(5000))
                CS_LOG_WARN(kCoreSaveLogDomain,
                             "journal pending stage=prepared slot=%d tx_id=%llu", slot,
                             journal.tx_id);
        }
        return fail(kQolErrBlocked, "prepared journal pending");
    }
    static CoreSaveLogThrottle unresolved_throttle;
    if (unresolved_throttle.due(5000))
        CS_LOG_WARN(kCoreSaveLogDomain, "journal unresolved slot=%d decode=%d", slot,
                     static_cast<int>(decoded));
    return fail(kQolErrBlocked, "unresolved save journal");
}
}  // namespace

QolResult save_transition_initialize(QolOwnerToken owner, QolOwnerVerifyTokenFn verify,
                                     const QolOwnerStoreApi* backend,
                                     const QolSlotOperationApi* operation,
                                     int32_t (*is_main)()) {
    if (owner == 0 || verify == nullptr || backend == nullptr || operation == nullptr ||
        is_main == nullptr || operation->acquire == nullptr || operation->release == nullptr ||
        backend->reset_in_operation == nullptr || backend->ensure_in_operation == nullptr)
        return fail(kQolErrInvalidArgument, "invalid transition deps");
    const auto identity = verify(owner);
    if (!succeeded(identity)) return identity;
    std::lock_guard<std::mutex> lock(g.mutex);
    g.owner = owner;
    g.verify = verify;
    g.backend = backend;
    g.operation = operation;
    g.is_main_thread = is_main;
    return ok();
}

void save_transition_shutdown() {
    std::lock_guard<std::mutex> lock(g.mutex);
    release_lease(g.pending);
    if (g.has_pending) {
        record_terminal(g.pending.id, TransitionResult{TransitionResultState::kCancelled,
                                                       g.pending.target_slot,
                                                       g.pending.old_epoch,
                                                       g.pending.old_epoch});
    }
    g.pending = PendingTransition{};
    g.has_pending = false;
    g.ready_published = false;
}

void save_transition_set_transition_gate(bool (*allowed)(int32_t, void*), void* ctx) {
    std::lock_guard<std::mutex> lock(g.mutex);
    g.transition_allowed = allowed;
    g.gate_ctx = ctx;
}

void save_transition_set_journal_verdict(QolResult (*verdict)(int32_t, const QolSlotOperationLease*, void*), void* ctx) {
    std::lock_guard<std::mutex> lock(g.mutex);
    g.journal_verdict = verdict;
    g.journal_ctx = ctx;
}

QolResult save_transition_begin(uint32_t kind, int32_t requested_slot, uint64_t* out_token) {
    if (out_token != nullptr) *out_token = 0;
    if (!main_thread()) {
        // begin 失败路径也经 catch-up 每帧重入：WARN 节流防刷屏。
        static CoreSaveLogThrottle throttle;
        if (throttle.due(5000))
            CS_LOG_WARN(kCoreSaveLogDomain,
                         "transition begin rejected reason=not_main_thread kind=%u slot=%d",
                         kind, requested_slot);
        return fail(kQolErrBlocked, "transition requires main thread");
    }
    if (out_token == nullptr || requested_slot < 0 || requested_slot > 2 ||
        kind > value(TransitionKind::kBeginNew)) {
        static CoreSaveLogThrottle throttle;
        if (throttle.due(5000))
            CS_LOG_WARN(kCoreSaveLogDomain,
                         "transition begin rejected reason=invalid_argument kind=%u slot=%d",
                         kind, requested_slot);
        return fail(kQolErrInvalidArgument, "invalid transition begin");
    }
    QolSlotSession old_session{-1, 0, 0};
    if (!succeeded(qol::features::core_save::slot_data_api().session(&old_session)))
        old_session = QolSlotSession{-1, value(QolSlotSessionState::kUnavailable), 0};
    // 锁内只采集失败现场，锁外记日志（日志不得进入 g.mutex 临界区）。
    enum class BeginBlock { kNone, kAlreadyPending, kSlotHealth, kTokenExhausted } block =
        BeginBlock::kNone;
    uint64_t pending_token = 0;
    uint32_t pending_kind = 0;
    int32_t pending_target = -1;
    uint32_t pending_phase = 0;
    bool pending_lease_held = false;
    {
        std::lock_guard<std::mutex> lock(g.mutex);
        if (g.has_pending) {
            block = BeginBlock::kAlreadyPending;
            pending_token = g.pending.id;
            pending_kind = value(g.pending.kind);
            pending_target = g.pending.target_slot;
            pending_phase = value(g.pending.phase);
            pending_lease_held = g.pending.lease_held;
        } else if (g.transition_allowed != nullptr &&
                   !g.transition_allowed(requested_slot, g.gate_ctx)) {
            block = BeginBlock::kSlotHealth;
        } else if (g.next_token == std::numeric_limits<uint64_t>::max()) {
            block = BeginBlock::kTokenExhausted;
        }
    }
    if (block == BeginBlock::kAlreadyPending) {
        static CoreSaveLogThrottle throttle;
        if (throttle.due(5000))
            CS_LOG_WARN(kCoreSaveLogDomain,
                         "transition begin rejected reason=already_pending kind=%u slot=%d "
                         "pending_token=%llu pending_kind=%u pending_target=%d "
                         "pending_phase=%u pending_lease_held=%d",
                         kind, requested_slot, pending_token, pending_kind, pending_target,
                         pending_phase, pending_lease_held ? 1 : 0);
        return fail(kQolErrBusy, "transition already pending");
    }
    if (block == BeginBlock::kSlotHealth) {
        static CoreSaveLogThrottle throttle;
        if (throttle.due(5000))
            CS_LOG_WARN(kCoreSaveLogDomain,
                         "transition begin rejected reason=slot_health_blocked kind=%u slot=%d",
                         kind, requested_slot);
        return fail(kQolErrBusy, "slot health blocks transition");
    }
    if (block == BeginBlock::kTokenExhausted) {
        static CoreSaveLogThrottle throttle;
        if (throttle.due(5000))
            CS_LOG_WARN(kCoreSaveLogDomain,
                         "transition begin rejected reason=token_exhausted kind=%u slot=%d",
                         kind, requested_slot);
        return fail(kQolErrInternal, "token exhausted");
    }
    // API 请求槽直接作为 target；不读 current_slot 代替。
    uint32_t mask = 1u << requested_slot;
    if (old_session.state != value(QolSlotSessionState::kUnavailable) && old_session.slot >= 0 &&
        old_session.slot <= 2)
        mask |= 1u << old_session.slot;  // 原子取得 old+target 两槽，一次 acquire。
    PendingTransition pending{};
    pending.lease.size = sizeof(pending.lease);
    QolResult acquired = ok();
    {
        std::lock_guard<std::mutex> lock(g.mutex);
        acquired = g.operation->acquire(g.operation->host_ctx, g.owner, mask, &pending.lease);
        if (succeeded(acquired)) {
            pending.lease_held = true;
            pending.id = g.next_token++;
            pending.kind = static_cast<TransitionKind>(kind);
            pending.target_slot = requested_slot;
            pending.old_epoch = old_session.epoch;
            pending.phase = TransitionPhase::kDetached;
        }
    }
    if (!succeeded(acquired)) {
        static CoreSaveLogThrottle throttle;
        if (throttle.due(5000))
            CS_LOG_WARN(kCoreSaveLogDomain,
                         "transition begin rejected reason=operation_busy kind=%u slot=%d "
                         "mask=0x%X code=%d message=%s",
                         kind, requested_slot, mask, acquired.code, log_message_of(acquired));
        return acquired;
    }
    // 关闭旧 session 准入；prepare_detach 在 provider 内部于基础设施锁之外执行。
    const uint32_t reason = value(QolSlotDataReason::kEnter);
    const auto began = qol::features::core_save::slot_data_begin_transition(reason, requested_slot);
    if (!succeeded(began)) {
        {
            std::lock_guard<std::mutex> lock(g.mutex);
            release_lease(pending);  // prepare_detach 拒绝：阻止原版转换，lease 已退。
        }
        static CoreSaveLogThrottle throttle;
        if (throttle.due(5000))
            CS_LOG_WARN(kCoreSaveLogDomain,
                         "transition begin rejected reason=slot_data_rejected kind=%u slot=%d "
                         "code=%d message=%s",
                         kind, requested_slot, began.code, log_message_of(began));
        return began;
    }
    {
        std::lock_guard<std::mutex> lock(g.mutex);
        g.pending = pending;
        g.has_pending = true;
        g.ready_published = false;  // 新转换期间事件状态不再 ready。
        *out_token = pending.id;
    }
    publish_save_ready(false, -1);
    CS_LOG_INFO(kCoreSaveLogDomain, "transition begun kind=%u slot=%d token=%llu old_epoch=%llu",
                 kind, requested_slot, pending.id, pending.old_epoch);
    return ok();
}

QolResult save_transition_confirm(uint64_t token_id, int32_t observed_slot) {
    if (!main_thread()) {
        static CoreSaveLogThrottle throttle;
        if (throttle.due(5000))
            CS_LOG_WARN(kCoreSaveLogDomain,
                         "transition confirm failed step=not_main_thread token=%llu target=%d",
                         token_id, observed_slot);
        return fail(kQolErrBlocked, "confirmation requires main thread");
    }
    // 锁内只采集失败现场，锁外记日志（节流防确认重试刷屏）。
    enum class ConfirmReject { kNone, kLookup, kSlotMismatch, kInFlight } reject = ConfirmReject::kNone;
    int32_t expected_target = -1;
    PendingTransition pending{};
    {
        std::lock_guard<std::mutex> lock(g.mutex);
        // 按 token id 显式复用；禁止按「槽相同」查找复用。
        if (!g.has_pending || g.pending.id != token_id) {
            reject = ConfirmReject::kLookup;
        } else if (observed_slot != g.pending.target_slot) {
            reject = ConfirmReject::kSlotMismatch;
            expected_target = g.pending.target_slot;
        } else if (g.pending.phase == TransitionPhase::kConfirming ||
                   g.pending.phase == TransitionPhase::kTerminal) {
            reject = ConfirmReject::kInFlight;
            expected_target = g.pending.target_slot;
        } else {
            pending = g.pending;
            pending.phase = TransitionPhase::kConfirming;
            g.pending.phase = TransitionPhase::kConfirming;
        }
    }
    if (reject == ConfirmReject::kLookup) {
        static CoreSaveLogThrottle throttle;
        if (throttle.due(5000))
            CS_LOG_WARN(kCoreSaveLogDomain,
                         "transition confirm failed step=lookup token=%llu target=%d code=%d "
                         "message=%s",
                         token_id, observed_slot, kQolErrNotFound, "transition token unknown");
        return fail(kQolErrNotFound, "transition token unknown");
    }
    if (reject == ConfirmReject::kSlotMismatch) {
        static CoreSaveLogThrottle throttle;
        if (throttle.due(5000))
            CS_LOG_WARN(kCoreSaveLogDomain,
                         "transition confirm failed step=slot_mismatch token=%llu observed=%d "
                         "target=%d code=%d message=%s",
                         token_id, observed_slot, expected_target, kQolErrConflict,
                         "observed slot mismatch");
        return fail(kQolErrConflict, "observed slot mismatch");
    }
    if (reject == ConfirmReject::kInFlight) {
        static CoreSaveLogThrottle throttle;
        if (throttle.due(5000))
            CS_LOG_WARN(kCoreSaveLogDomain,
                         "transition confirm failed step=in_flight token=%llu target=%d code=%d "
                         "message=%s",
                         token_id, expected_target, kQolErrBusy, "confirmation in flight");
        return fail(kQolErrBusy, "confirmation in flight");
    }
    // 确认序第 1 步：新档 reset / 读档 ensure。均用已校验的请求槽。
    const auto prepared =
        pending.kind == TransitionKind::kBeginNew
            ? g.backend->reset_in_operation(g.owner, &pending.lease, pending.target_slot)
            : g.backend->ensure_in_operation(g.owner, &pending.lease, pending.target_slot);
    if (!succeeded(prepared)) {
        {
            std::lock_guard<std::mutex> lock(g.mutex);
            // 不消费 pending：token 与 lease 保留，可重试确认或取消。
            g.pending.phase = TransitionPhase::kAwaitingWorld;
        }
        static CoreSaveLogThrottle throttle;
        if (throttle.due(5000))
            CS_LOG_WARN(kCoreSaveLogDomain,
                         "transition confirm failed step=ensure token=%llu target=%d code=%d "
                         "message=%s",
                         token_id, pending.target_slot, prepared.code, log_message_of(prepared));
        return prepared;
    }
    // 第 2 步：未决 journal 严格分派（不盲目抹证据）；阻断则保持 pending。
    const auto verdict = journal_check(pending.target_slot, &pending.lease);
    if (!succeeded(verdict)) {
        {
            std::lock_guard<std::mutex> lock(g.mutex);
            g.pending.phase = TransitionPhase::kAwaitingWorld;
        }
        static CoreSaveLogThrottle throttle;
        if (throttle.due(5000))
            CS_LOG_WARN(kCoreSaveLogDomain,
                         "transition confirm failed step=journal token=%llu target=%d code=%d "
                         "message=%s",
                         token_id, pending.target_slot, verdict.code, log_message_of(verdict));
        return verdict;
    }
    // 第 3..n 步：新 epoch → session_ready 通知 → SessionBound 加载 / Immediate 排空 →
    // ready 发布（provider confirm 内部完成 activate 派发与 pump）。
    const auto confirmed = qol::features::core_save::slot_data_confirm_transition(observed_slot);
    if (!succeeded(confirmed)) {
        {
            std::lock_guard<std::mutex> lock(g.mutex);
            g.pending.phase = TransitionPhase::kAwaitingWorld;
        }
        static CoreSaveLogThrottle throttle;
        if (throttle.due(5000))
            CS_LOG_WARN(kCoreSaveLogDomain,
                         "transition confirm failed step=activate token=%llu target=%d code=%d "
                         "message=%s",
                         token_id, pending.target_slot, confirmed.code,
                         log_message_of(confirmed));
        return confirmed;
    }
    QolSlotSession ready_session{-1, 0, 0};
    (void)qol::features::core_save::slot_data_api().session(&ready_session);
    {
        std::lock_guard<std::mutex> lock(g.mutex);
        record_terminal(token_id, TransitionResult{TransitionResultState::kReady,
                                                   pending.target_slot, pending.old_epoch,
                                                   ready_session.epoch});
        release_lease(g.pending);
        g.pending.phase = TransitionPhase::kTerminal;
        g.pending = PendingTransition{};
        g.has_pending = false;
        // 此后事件状态才可发布 ready。
        g.ready_published = true;
    }
    publish_save_ready(true, pending.target_slot);
    CS_LOG_INFO(kCoreSaveLogDomain,
                 "transition ready token=%llu slot=%d old_epoch=%llu epoch=%llu", token_id,
                 pending.target_slot, pending.old_epoch, ready_session.epoch);
    return ok();
}

QolResult save_transition_cancel(uint64_t token_id) {
    if (!main_thread()) return fail(kQolErrBlocked, "cancel requires main thread");
    bool unknown = false;
    int32_t cancelled_target = -1;
    {
        std::lock_guard<std::mutex> lock(g.mutex);
        if (!g.has_pending || g.pending.id != token_id) {
            unknown = true;
        } else {
            const PendingTransition cancelled = g.pending;
            cancelled_target = cancelled.target_slot;
            record_terminal(token_id, TransitionResult{TransitionResultState::kCancelled,
                                                       cancelled.target_slot,
                                                       cancelled.old_epoch,
                                                       cancelled.old_epoch});
            release_lease(g.pending);
            g.pending = PendingTransition{};
            g.has_pending = false;
            // 只释放 token/lease；已删 dat 不伪装恢复（provider 撤销 transitioning，
            // session 回 Unavailable）。保持原锁序：abandon 仍在 g.mutex 内执行，
            // 不与并发 begin 的 provider transitioning 窗口交错。
            (void)qol::features::core_save::slot_data_abandon_transition();
        }
    }
    publish_save_ready(false, -1);
    if (unknown) {
        CS_LOG_WARN(kCoreSaveLogDomain, "transition cancel failed token=%llu step=lookup", token_id);
        return fail(kQolErrNotFound, "transition token unknown");
    }
    CS_LOG_INFO(kCoreSaveLogDomain, "transition cancelled token=%llu slot=%d", token_id,
                 cancelled_target);
    return ok();
}

QolResult save_transition_query(uint64_t token_id, TransitionResult* out) {
    if (out == nullptr || token_id == 0) return fail(kQolErrInvalidArgument, "invalid token query");
    std::lock_guard<std::mutex> lock(g.mutex);
    if (g.has_pending && g.pending.id == token_id) {
        *out = TransitionResult{TransitionResultState::kInProgress, g.pending.target_slot,
                                g.pending.old_epoch, g.pending.old_epoch};
        return ok();
    }
    const auto terminal = g.terminal.find(token_id);
    if (terminal != g.terminal.end()) {
        *out = terminal->second;
        return ok();
    }
    *out = TransitionResult{TransitionResultState::kFailed, -1, 0, 0};
    return ok();
}

bool save_transition_ready_for_save_enter() {
    std::lock_guard<std::mutex> lock(g.mutex);
    return g.ready_published;
}

bool save_transition_pending_observation(uint64_t* out_token, int32_t* out_target) {
    if (out_token != nullptr) *out_token = 0;
    if (out_target != nullptr) *out_target = -1;
    std::lock_guard<std::mutex> lock(g.mutex);
    if (!g.has_pending) return false;
    // 确认中/终态不重复确认；detached（等选角/等原版转换）→ executing/awaiting world
    // 期间均由 RenderPre 观测驱动确认。
    if (g.pending.phase == TransitionPhase::kConfirming ||
        g.pending.phase == TransitionPhase::kTerminal)
        return false;
    if (out_token != nullptr) *out_token = g.pending.id;
    if (out_target != nullptr) *out_target = g.pending.target_slot;
    return true;
}

bool save_transition_bypass_lease(contracts::QolSlotOperationLease* out_lease,
                                  int32_t* out_target) {
    if (out_lease != nullptr) *out_lease = contracts::QolSlotOperationLease{};
    if (out_target != nullptr) *out_target = -1;
    std::lock_guard<std::mutex> lock(g.mutex);
    // 只有读档转换提供 loading 旁路凭据：新档 selection 期间 SAVE_Save 无旁路语义。
    if (!g.has_pending || g.pending.kind != TransitionKind::kResume || !g.pending.lease_held)
        return false;
    if (g.pending.phase == TransitionPhase::kConfirming ||
        g.pending.phase == TransitionPhase::kTerminal)
        return false;
    if (out_lease != nullptr) *out_lease = g.pending.lease;
    if (out_target != nullptr) *out_target = g.pending.target_slot;
    return true;
}

QolResult save_transition_prepare_stop() {
    std::lock_guard<std::mutex> lock(g.mutex);
    // 无未决转换才放行；未决事务的健康由协调器 gate 在 begin 时表达。
    if (g.has_pending) return fail(kQolErrBusy, "transition pending");
    return ok();
}

bool save_transition_confirm_throttled(uint64_t last_token, uint64_t token, uint64_t last_frame,
                                       uint64_t now, uint64_t window) {
    if (last_frame == 0 || last_token != token) return false;  // 尚未执行或新 token：放行。
    if (now < last_frame) return false;  // 回绕按窗口已届满处理，不继续节流。
    return now - last_frame < window;
}

TransitionTimeoutTick save_transition_timeout_tick(bool timer_started, uint64_t* elapsed_frames,
                                                   uint64_t* last_frame,
                                                   uint64_t* backward_window_begin,
                                                   uint32_t* backward_count, uint64_t now,
                                                   uint64_t timeout, uint32_t backward_limit) {
    if (!timer_started || elapsed_frames == nullptr || last_frame == nullptr ||
        backward_window_begin == nullptr ||
        backward_count == nullptr || backward_limit == 0)
        return TransitionTimeoutTick::kWaiting;
    if (now < *last_frame) {
        if (*backward_count == 0 ||
            (now >= *backward_window_begin && now - *backward_window_begin > timeout)) {
            *backward_window_begin = now;
            *backward_count = 1;
        } else if (*backward_count < backward_limit) {
            ++*backward_count;
        }
        *last_frame = now;
        // 单次倒退可能是乱序帧；多次倒退则无法由单次抖动解释，视为异常时钟。
        return *backward_count >= backward_limit ? TransitionTimeoutTick::kExpired
                                                  : TransitionTimeoutTick::kRebased;
    }
    if (*backward_count != 0 && now >= *backward_window_begin &&
        now - *backward_window_begin > timeout)
        *backward_count = 0;
    const uint64_t forward_delta = now - *last_frame;
    *elapsed_frames = forward_delta > UINT64_MAX - *elapsed_frames
                          ? UINT64_MAX
                          : *elapsed_frames + forward_delta;
    *last_frame = now;
    if (*elapsed_frames >= timeout) return TransitionTimeoutTick::kExpired;
    return TransitionTimeoutTick::kWaiting;
}

void save_transition_reset_for_test() {
    std::lock_guard<std::mutex> lock(g.mutex);
    release_lease(g.pending);
    g.pending = PendingTransition{};
    g.has_pending = false;
    g.ready_published = false;
    g.next_token = 1;
    g.terminal.clear();
    g.terminal_order.clear();
    g.transition_allowed = nullptr;
    g.gate_ctx = nullptr;
    g.journal_verdict = nullptr;
    g.journal_ctx = nullptr;
}
}  // namespace qol::features::core_save
