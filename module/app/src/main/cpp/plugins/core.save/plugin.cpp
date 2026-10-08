#include "plugin.h"
#include "save_coordinator.h"
#include "save_transition.h"
#include "slot_data_provider.h"

#include <atomic>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <string>

#include "contracts/commands.h"
#include "contracts/game_ports/save_port.h"
#include "contracts/game_ports/save_transaction_port.h"
#include "contracts/game_ports/save_write_port.h"
#include "contracts/http.h"
#include "contracts/save_facts.h"
#include "contracts/save_route.h"
#include "contracts/runtime_hooks.h"

namespace qol::features::core_save {
namespace {

constexpr const char* kPluginId = "core.save";
constexpr const char* kPluginVersion = "0.1.0";
[[gnu::used]] constexpr char kSaveFactsBuildMarker[] = "save.facts:v1";

const contracts::QolSavePort* g_port = nullptr;

QolResult not_ready() {
    return contracts::error(kQolErrNotReady, "save facts not ready", 20);
}

QolResult slot_state(int32_t slot, int32_t* out_state) {
    if (g_port == nullptr || g_port->read.slot_state == nullptr) return not_ready();
    return g_port->read.slot_state(slot, out_state);
}
QolResult slot_exists(int32_t slot, int32_t* out_exists) {
    if (g_port == nullptr || g_port->read.slot_exists == nullptr) return not_ready();
    return g_port->read.slot_exists(slot, out_exists);
}
QolResult slot_hero_index(int32_t slot, int32_t* out_index) {
    if (g_port == nullptr || g_port->read.slot_hero_index == nullptr) return not_ready();
    return g_port->read.slot_hero_index(slot, out_index);
}
QolResult hero_level(int32_t slot, int32_t* out_level) {
    if (g_port == nullptr || g_port->read.hero_level == nullptr) return not_ready();
    return g_port->read.hero_level(slot, out_level);
}
QolResult hero_class(int32_t slot, int32_t* out_class) {
    if (g_port == nullptr || g_port->read.hero_class == nullptr) return not_ready();
    return g_port->read.hero_class(slot, out_class);
}
QolResult load_data(int32_t slot, void** out_data, int32_t* out_size) {
    if (g_port == nullptr || g_port->read.load_data == nullptr) return not_ready();
    return g_port->read.load_data(slot, out_data, out_size);
}
QolResult hub_key(const char** out_key) {
    if (g_port == nullptr || g_port->read.hub_key == nullptr) return not_ready();
    return g_port->read.hub_key(out_key);
}
QolResult map_id(int32_t slot, int32_t* out_map_id) {
    if (g_port == nullptr || g_port->read.map_id == nullptr) return not_ready();
    return g_port->read.map_id(slot, out_map_id);
}
QolResult current_slot(int32_t* out_slot) {
    if (g_port == nullptr || g_port->read.current_slot == nullptr) return not_ready();
    return g_port->read.current_slot(out_slot);
}
QolResult is_ok(int32_t* out_ok) {
    if (g_port == nullptr || g_port->read.is_ok == nullptr) return not_ready();
    return g_port->read.is_ok(out_ok);
}

constexpr contracts::SaveFactsApi kSaveFactsApi{
    &slot_state, &slot_exists, &slot_hero_index, &hero_level, &hero_class,
    &load_data, &hub_key, &map_id, &current_slot, &is_ok,
};

const contracts::CapabilityProvisionRef kProvides[] = {
    {contracts::kSaveFactsCapabilityId, contracts::kSaveFactsVersion, &kSaveFactsApi},
    {contracts::kQolSlotDataCapabilityId, contracts::kQolSlotDataVersion, &slot_data_api()},
};

const contracts::QolPortRequirement kPorts[] = {
    {contracts::QolPortId::QOL_PORT_SAVE,
     {contracts::kQolPortSaveVersion.major, contracts::kQolPortSaveVersion.minor}},
    {contracts::QolPortId::QOL_PORT_SAVE_WRITE,
     {contracts::kQolPortSaveWriteVersion.major, contracts::kQolPortSaveWriteVersion.minor}},
    {contracts::QolPortId::QOL_PORT_SAVE_TRANSACTION,
     {contracts::kQolPortSaveTransactionVersion.major,
      contracts::kQolPortSaveTransactionVersion.minor}},
};

void log_message(const contracts::QolHostSdk* sdk, const char* message) {
    if (sdk == nullptr || sdk->log.write == nullptr || message == nullptr) return;
    uint32_t length = 0;
    while (message[length] != '\0') ++length;
    (void)sdk->log.write(1, 1, kPluginId, message, length);
}

// ---- P3c 生产切换：SAVE_Save 入口 / 转换状态机接线 ----
// 全部原版与自动保存经 hook:SAVE_Save 进入唯一联合协调器；读档/新档入口先进入转换
// 状态机（prepare_detach → RenderPre 确认序 → ready）。loading 旁路按 P3b-2 准入矩阵
// 经 Adapter 探针判定，仅在读档转换窗口内放行。

const contracts::QolSaveTransactionPort* g_save_tx = nullptr;
const contracts::QolHostSdk* g_sdk = nullptr;
const contracts::QolGameHost* g_game = nullptr;
contracts::QolOwnerToken g_owner = 0;
uint64_t g_save_route_binding = 0;
uint64_t g_slot_data_frame_task = 0;
void bind_transition_ticket_context(uint64_t token, int32_t kind, int32_t slot);

// RenderPre 帧号（确认序观测与恢复重试共用同一帧源）。
std::atomic<uint64_t> g_frame{0};
// LogicPre 帧号：失败等待（读档确认超时）在此持续推进，不依赖仅 world 态执行的 RenderPre。
std::atomic<uint64_t> g_logic_frame{0};
uint64_t g_logic_clock_task = 0;
// 确认序重试节流：上次实际执行 confirm 的帧与 token。
std::atomic<uint64_t> g_last_confirm_attempt{0};
std::atomic<uint64_t> g_last_confirm_token{0};
// 新档 selection token：GoToNewGame 入口 begin 分配，确认序按 token id 复用，取消释放。
std::atomic<uint64_t> g_selection_token{0};
// kResume begin 的帧号（确认超时取消用；读档失败不悬挂 pending 转换）。
std::atomic<uint64_t> g_resume_begin_frame{0};
std::atomic<bool> g_resume_timer_started{false};
std::atomic<uint64_t> g_resume_timer_token{0};
// kResume 挂起超时：约 10 秒（30fps）；世界未就绪且超时即取消，防后续 begin Busy 死锁。
constexpr uint64_t kResumeConfirmTimeoutFrames = 600;

// ---- 资源 handler 的原版 trampoline（on_resource_ready 常数时间绑定）----
using SaveSaveFn = int (*)();
using GoToNewGameFn = void (*)(int32_t);
using SelectCharBackFn = void (*)();
SaveSaveFn g_backup_save_save = nullptr;
GoToNewGameFn g_backup_goto_new_game = nullptr;
SelectCharBackFn g_backup_select_back = nullptr;

bool main_thread_now() {
    return g_game != nullptr && g_game->thread.is_main_thread != nullptr &&
           g_game->thread.is_main_thread() != 0;
}

// ---- save_route 领域回调（宿主锁外、主线程执行）----

QolResult cs_native_call(void* /*user*/, const contracts::QolSaveNativeCall* call,
                         contracts::QolSaveDomainStatus* out) {
    if (call == nullptr || call->size < sizeof(*call))
        return contracts::error(kQolErrInvalidArgument, "invalid native call", 17);
    if (out != nullptr) {
        out->size = sizeof(*out);
        out->state = static_cast<uint32_t>(contracts::QolSaveState::kPending);
        out->outcome = 0;
        out->slot = call->target_slot;
        out->epoch = 0;
        out->transaction = 0;
        out->transition = 0;
        out->original.size = sizeof(out->original);
        out->original.invoked = 0;
        out->original.result_valid = 0;
        out->original.original_result = 0;
    }
    const auto kind = static_cast<contracts::QolSaveCallKind>(call->kind);
    switch (kind) {
        case contracts::QolSaveCallKind::kSave: {
            // 联合保存事务：SAVE_Save 的普通保存路径（session ready）。
            std::string body;
            const auto settled = save_coordinator_save_entry(
                call->target_slot, 0, g_frame.load(), out != nullptr ? &out->original : nullptr,
                &body);
            if (out != nullptr) {
                out->state = static_cast<uint32_t>(
                    settled == qol::features::core_save::SaveSettle::kCommitted
                        ? contracts::QolSaveState::kDone
                        : settled == qol::features::core_save::SaveSettle::kPending
                              ? contracts::QolSaveState::kPending
                              : contracts::QolSaveState::kFailed);
            }
            return settled == qol::features::core_save::SaveSettle::kFailed
                       ? contracts::error(kQolErrBlocked, "save transaction failed", 21)
                       : contracts::ok();
        }
        case contracts::QolSaveCallKind::kResume:
        case contracts::QolSaveCallKind::kBeginNew: {
            // 读档/新档发起：begin_transition（target 为调用方已校验的请求槽）。
            uint64_t token = 0;
            const auto began = save_transition_begin(
                kind == contracts::QolSaveCallKind::kResume
                    ? static_cast<uint32_t>(TransitionKind::kResume)
                    : static_cast<uint32_t>(TransitionKind::kBeginNew),
                call->target_slot, &token);
            if (out != nullptr) out->transition = token;
            if (kind == contracts::QolSaveCallKind::kResume && contracts::succeeded(began)) {
                bind_transition_ticket_context(token, contracts::kQolSaveWriteEnterSlot,
                                               call->target_slot);
                g_resume_begin_frame.store(g_logic_frame.load(), std::memory_order_release);
                g_resume_timer_token.store(token, std::memory_order_release);
                g_resume_timer_started.store(true, std::memory_order_release);
            }
            if (kind == contracts::QolSaveCallKind::kBeginNew && contracts::succeeded(began)) {
                bind_transition_ticket_context(token, contracts::kQolSaveWriteCreateSlot,
                                               call->target_slot);
                g_selection_token.store(token, std::memory_order_release);
            }
            return began;
        }
        case contracts::QolSaveCallKind::kMaintenance:
            // save_enter readiness 探针：确认序发布 ready 后才允许旧回调与事件派发。
            return save_transition_ready_for_save_enter()
                       ? contracts::ok()
                       : contracts::error(kQolErrBlocked, "save enter not ready", 18);
        case contracts::QolSaveCallKind::kCancelTransition:
            // 原版未启动/明确失败的出口取消未决转换（释放 token/lease，不伪装恢复已删 dat）。
            return save_transition_cancel(call->transition);
        default:
            return contracts::error(kQolErrInvalidArgument, "unsupported save call kind", 24);
    }
}

QolResult cs_render_ready(void* /*user*/, uint64_t /*transition*/,
                          const contracts::QolSaveAdapterState* /*state*/,
                          contracts::QolSaveDomainStatus* /*out*/) {
    // Game 层 thunk 目前只经 native_call 派发；render_ready 由 Adapter 观测驱动，
    // core.save 确认序自行读取 Adapter 状态（save_transition_observe），无独立消费者。
    return contracts::error(kQolErrUnsupported, "render ready unused", 18);
}

QolResult cs_prepare_stop(void* /*user*/) {
    // 停用前置（§9）：存在未决转换，或任一槽健康非 healthy（未决事务/降级）时 Busy。
    const auto prepared = save_transition_prepare_stop();
    if (!contracts::succeeded(prepared)) return prepared;
    for (int32_t slot = 0; slot <= 2; ++slot) {
        if (!save_coordinator_permits_transition(slot))
            return contracts::error(kQolErrBusy, "save transaction pending", 22);
    }
    return contracts::ok();
}

// RenderPre 确认序观测：世界就绪且观测槽匹配时按 token id 复用确认；kResume 挂起
// 超时自动取消（读档失败/放弃不悬挂 pending，避免后续 begin 永久 Busy）。
// confirm 失败（journal 阻断/ensure 失败）保留 pending 由后续帧重试，30 帧节流避免
// 每帧 reset/ensure 的 IO 压力。
void save_transition_observe() {
    const uint64_t now = g_logic_frame.load();
    // 当前 world 的受控 catch-up/重新激活：session 非 ready 且无未决转换、Adapter 观测到
    // 世界就绪时，按当前槽发起 resume 转换（ensure+加载现存 sidecar，不执行新档 reset）。
    // 覆盖「已进档时启用/停用后再启用」的当前档补加载，加载前操作门保持关闭。
    if (g_save_tx != nullptr && g_save_tx->read_state != nullptr) {
        contracts::QolSlotSession current{
            -1, static_cast<uint32_t>(contracts::QolSlotSessionState::kUnavailable), 0};
        if (contracts::succeeded(slot_data_api().session(&current)) &&
            current.state != static_cast<uint32_t>(contracts::QolSlotSessionState::kReady)) {
            uint64_t pending_token = 0;
            int32_t pending_target = -1;
            if (!save_transition_pending_observation(&pending_token, &pending_target)) {
                contracts::QolSaveAdapterState st{};
                st.size = sizeof(st);
                if (contracts::succeeded(g_save_tx->read_state(g_save_tx->adapter_ctx, &st)) &&
                    st.world_ready != 0 && st.current_slot >= 0 && st.current_slot <= 2) {
                    uint64_t token = 0;
                    (void)save_transition_begin(static_cast<uint32_t>(TransitionKind::kResume),
                                                st.current_slot, &token);
                }
            }
        }
    }
    uint64_t token = 0;
    int32_t target = -1;
    if (!save_transition_pending_observation(&token, &target)) return;
    if (g_save_tx == nullptr || g_save_tx->read_state == nullptr) return;
    contracts::QolSaveAdapterState state{};
    state.size = sizeof(state);
    const auto read = g_save_tx->read_state(g_save_tx->adapter_ctx, &state);
    if (!contracts::succeeded(read) || state.world_ready == 0 || state.current_slot != target) {
        // 超时取消只对读档路径：新档 selection 可无限期等待用户操作（Back/确认结束）。
        return;
    }
    g_resume_begin_frame.store(0, std::memory_order_release);
    g_resume_timer_started.store(false, std::memory_order_release);
    g_resume_timer_token.store(0, std::memory_order_release);
    // 节流仅在实际执行 confirm 后更新；首次尝试与新 token 独立放行，首次失败后可立即重试。
    const uint64_t last_token = g_last_confirm_token.load(std::memory_order_acquire);
    const uint64_t last = g_last_confirm_attempt.load(std::memory_order_acquire);
    if (save_transition_confirm_throttled(last_token, token, last, now, 30))
        return;  // 同一 token 上次实际执行不足 30 帧前。
    g_last_confirm_token.store(token, std::memory_order_release);
    g_last_confirm_attempt.store(now, std::memory_order_release);
    (void)save_transition_confirm(token, state.current_slot);
}

void save_transition_timeout_tick(uint64_t now) {
    if (!g_resume_timer_started.load(std::memory_order_acquire)) return;
    uint64_t token = 0;
    int32_t target = -1;
    if (!save_transition_pending_observation(&token, &target) ||
        token != g_resume_timer_token.load(std::memory_order_acquire)) return;
    const uint64_t began = g_resume_begin_frame.load(std::memory_order_acquire);
    // A backwards frame counter is treated as a new time base.
    if (!save_transition_timeout_elapsed(true, began, now, kResumeConfirmTimeoutFrames)) return;
    if (contracts::succeeded(save_transition_cancel(token))) {
        g_resume_begin_frame.store(0, std::memory_order_release);
        g_resume_timer_token.store(0, std::memory_order_release);
        g_resume_timer_started.store(false, std::memory_order_release);
    }
}

int32_t slot_data_frame(int64_t frame, void*) {
    g_frame.store(static_cast<uint64_t>(frame), std::memory_order_release);
    slot_data_pump();
    save_transition_observe();
    // 生产帧任务推进 hook 入口发起且仍 pending 的保存事务（HTTP 票据不重复推进）。
    save_coordinator_pump(g_frame.load());
    return 1;
}

// LogicPre 时钟：非 world 态（RenderPre 不执行）也持续运行，供确认超时等待。
int32_t logic_clock_frame(int64_t frame, void*) {
    const auto now = static_cast<uint64_t>(frame);
    g_logic_frame.store(now, std::memory_order_release);
    save_transition_timeout_tick(now);
    return 1;
}

// ---- 资源 handler（游戏直接调用；trampoline 为 hook 引擎交还原函数）----

// hook:SAVE_Save handler。游戏可见 int = 原版真实返回（invoked=1）；
// 协调器未执行原版（prepare 失败/旁路拒绝）返回 0（invoked=0，fail-closed）。
int save_save_wrapper() {
    if (!main_thread_now()) return 0;
    contracts::QolSlotSession session{-1, 0, 0};
    const bool ready = contracts::succeeded(slot_data_api().session(&session)) &&
                       session.state == static_cast<uint32_t>(
                           contracts::QolSlotSessionState::kReady) &&
                       save_coordinator_installed();
    if (!ready) {
        // session 非 ready：SAVE_Save 只在读档转换窗口内有合法语境（SAVE_LoadCharacterAll
        // 内部 bl）。按 loading 旁路矩阵经 Adapter 探针判定；拒绝/无凭据一律 0。
        if (g_save_tx == nullptr || g_save_tx->probe_loading_bypass == nullptr) return 0;
        contracts::QolSlotOperationLease lease{};
        int32_t target = -1;
        if (!save_transition_bypass_lease(&lease, &target)) return 0;
        contracts::QolSaveOriginalResult original{};
        original.size = sizeof(original);
        const auto probed = g_save_tx->probe_loading_bypass(g_save_tx->adapter_ctx, &lease, target);
        if (!contracts::succeeded(probed)) return 0;
        // 旁路放行：直接经 Adapter trampoline 执行原版（与普通保存共用受控入口；
        // Adapter 在此窗口标记 joint in-flight，拒绝嵌套事务）。
        const auto invoked = g_save_tx->call_original_save(g_save_tx->adapter_ctx, &lease, target,
                                                           &original);
        if (!contracts::succeeded(invoked) || original.invoked == 0) return 0;
        return original.result_valid != 0 ? original.original_result : 0;
    }
    // 普通保存：经宿主 save_route 派发进入唯一协调器（owner 停用后派发被摘除）。
    if (g_sdk == nullptr || g_sdk->save_route == nullptr ||
        g_sdk->save_route->dispatch == nullptr)
        return 0;
    contracts::QolSaveNativeCall call{};
    call.size = sizeof(call);
    call.kind = static_cast<uint32_t>(contracts::QolSaveCallKind::kSave);
    call.target_slot = session.slot;
    call.class_index = -1;
    call.origin = static_cast<uint32_t>(contracts::QolSaveOrigin::kOrdinary);
    contracts::QolSaveDomainStatus status{};
    status.size = sizeof(status);
    const auto routed =
        g_sdk->save_route->dispatch(g_sdk->save_route->host_ctx, g_owner, &call, &status);
    // 游戏可见 int = 原版真实返回；原版未执行（invoked=0）或派发失败返回 0。
    if (routed.code == kQolErrNotFound) return 0;  // binding 已摘除：不执行。
    if (status.original.invoked == 0) return 0;
    return status.original.result_valid != 0 ? status.original.original_result : 0;
}

// hook:SaveSlot_GoToNewGame handler：入口 begin（kBeginNew）；准入关闭（槽健康/转换
// 在途/prepare_detach 拒绝）不进入新档流程（fail-closed，不执行删档与状态写）。
// 重入（selection 悬挂后再点空槽）：先取消旧 selection token 再 begin。
void goto_new_game_wrapper(int32_t slot) {
    if (!main_thread_now() || slot < 0 || slot > 2) return;
    uint64_t token = 0;
    auto began = save_transition_begin(static_cast<uint32_t>(TransitionKind::kBeginNew), slot,
                                       &token);
    if (!contracts::succeeded(began)) {
        const uint64_t stale = g_selection_token.exchange(0, std::memory_order_acq_rel);
        if (stale != 0) (void)save_transition_cancel(stale);
        began = save_transition_begin(static_cast<uint32_t>(TransitionKind::kBeginNew), slot,
                                      &token);
    }
    if (!contracts::succeeded(began)) {
        log_message(g_sdk, "[core.save] new game begin rejected; original transition blocked");
        return;
    }
    g_selection_token.store(token, std::memory_order_release);
    if (g_backup_goto_new_game != nullptr) g_backup_goto_new_game(slot);
}

// hook:SelectCharacter_ButtonBackExe handler：选角取消只释放 selection token/lease
//（已删 dat 不伪装恢复；provider 撤销 transitioning，session 回 Unavailable）。
void select_char_back_wrapper() {
    const uint64_t token = g_selection_token.exchange(0, std::memory_order_acq_rel);
    if (token != 0) (void)save_transition_cancel(token);
    if (g_backup_select_back != nullptr) g_backup_select_back();
}

bool save_transition_gate_admits(int32_t slot, void* /*ctx*/) {
    return save_coordinator_permits_transition(slot);
}

QolResult initialize_slot_data(const contracts::PluginContext& ctx) {
    if (ctx.struct_size < sizeof(contracts::PluginContext) || ctx.sdk == nullptr ||
        ctx.sdk->abi.size < sizeof(contracts::QolHostSdk) || ctx.game == nullptr)
        return not_ready();
    return slot_data_initialize(ctx.owner_token, ctx.sdk->owner_verify_token, ctx.sdk->owner_store,
                                ctx.game->thread.is_main_thread);
}

// ---- 存档域写面（阶段 5 票据契约）----
// HTTP 线程只校验并投递票据；游戏写在主线程结果处理器执行（含模块 sidecar 生命周期与
// 转场后的世界态等待），结果经票据回取。owner 与读面同属 core.save（一个领域一个 owner）。
enum class SaveWriteRoute : uint8_t { kSave, kEnterSlot, kCreateSlot };

// typed handler id 为全局空间：core.save 沿用原 save 写 id（随代码从 save_backup 迁入）。
constexpr uint32_t kSaveWriteHandlerId = 0x53420001;

struct SaveWritePayload {
    int32_t kind = 0;  // contracts::kQolSaveWrite*
    int32_t slot = -1;
    int32_t class_idx = -1;
    uint64_t request_sequence = 0;
};

struct SaveTransitionTicketContext {
    int32_t kind = 0;
    int32_t slot = -1;
    uint64_t token = 0;
};
std::mutex g_transition_ticket_mutex;
std::map<uint64_t, SaveTransitionTicketContext> g_transition_ticket_contexts;
std::atomic<uint64_t> g_next_save_request_sequence{1};
thread_local uint64_t g_active_save_request_sequence = 0;

void bind_transition_ticket_context(uint64_t token, int32_t kind, int32_t slot) {
    const uint64_t sequence = g_active_save_request_sequence;
    if (token == 0 || sequence == 0) return;
    std::lock_guard<std::mutex> lock(g_transition_ticket_mutex);
    g_transition_ticket_contexts[sequence] = SaveTransitionTicketContext{kind, slot, token};
}

bool transition_ticket_context(uint64_t sequence, int32_t kind, int32_t slot,
                                SaveTransitionTicketContext* out) {
    if (sequence == 0 || out == nullptr) return false;
    std::lock_guard<std::mutex> lock(g_transition_ticket_mutex);
    const auto it = g_transition_ticket_contexts.find(sequence);
    if (it == g_transition_ticket_contexts.end() || it->second.kind != kind ||
        it->second.slot != slot) return false;
    *out = it->second;
    return true;
}

void clear_transition_ticket_context(uint64_t sequence) {
    std::lock_guard<std::mutex> lock(g_transition_ticket_mutex);
    g_transition_ticket_contexts.erase(sequence);
}

void clear_all_transition_ticket_contexts() {
    std::lock_guard<std::mutex> lock(g_transition_ticket_mutex);
    g_transition_ticket_contexts.clear();
}

const contracts::QolSaveWritePort* g_save_write = nullptr;

QolResult cs_http_error(contracts::HttpRes* response, uint16_t status, const char* message) {
    const std::string body = std::string("{\"ok\":false,\"error\":\"") + message + "\"}";
    if (response == nullptr || response->body == nullptr ||
        response->body_capacity < body.size()) {
        return contracts::error(kQolErrInternal, "http response buffer too small", 31);
    }
    std::memcpy(response->body, body.data(), body.size());
    response->body_size = static_cast<uint32_t>(body.size());
    response->status = status;
    return contracts::error(status == 400 ? kQolErrInvalidArgument : kQolErrInternal, message,
                            static_cast<uint32_t>(std::strlen(message)));
}

QolResult cs_http_body(contracts::HttpRes* response, const std::string& body) {
    if (response == nullptr || response->body == nullptr ||
        response->body_capacity < body.size()) {
        return cs_http_error(response, 500, "http response buffer too small");
    }
    std::memcpy(response->body, body.data(), body.size());
    response->body_size = static_cast<uint32_t>(body.size());
    response->status = 200;
    return contracts::ok();
}

// HttpReq.body 无 null 结尾契约；调用方已拷贝为 std::string，此处只做字段提取。
bool cs_extract_int(const char* body, const char* key, int* out) {
    const std::string pattern = std::string("\"") + key + "\":";
    const char* p = std::strstr(body, pattern.c_str());
    if (p == nullptr) return false;
    p += pattern.size();
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') ++p;
    char* end = nullptr;
    const long value = std::strtol(p, &end, 10);
    if (end == p) return false;
    *out = static_cast<int>(value);
    return true;
}

// 主线程命令体：执行存档写（含模块 sidecar 生命周期），再生成路由专属成功结果。
bool save_write_result_handler(void*, const uint8_t* payload, uint32_t size, char* out,
                               uint32_t capacity, uint32_t* out_size) {
    if (out_size != nullptr) *out_size = 0;
    if (payload == nullptr || size != sizeof(SaveWritePayload) || out == nullptr ||
        out_size == nullptr || g_save_write == nullptr || g_save_write->read.write == nullptr) {
        return false;
    }
    SaveWritePayload p{};
    std::memcpy(&p, payload, size);
    // 命令执行次数（首次 0，延迟重试递增）：端口据此区分「首次发起转换」与「等待世界态就绪」。
    const uint32_t attempt =
        (g_sdk != nullptr && g_sdk->ticket_result.current_attempt != nullptr)
            ? g_sdk->ticket_result.current_attempt()
            : 0;
    const bool transition_write = p.kind == contracts::kQolSaveWriteEnterSlot ||
                                  p.kind == contracts::kQolSaveWriteCreateSlot;
    const auto fail_transition = [&](const char* reason) {
        const std::string body = std::string("{\"ok\":false,\"error\":\"") + reason + "\"}";
        clear_transition_ticket_context(p.request_sequence);
        if (body.size() > capacity) return false;
        std::memcpy(out, body.data(), body.size());
        *out_size = static_cast<uint32_t>(body.size());
        return false;
    };
    const auto defer_transition = [&]() {
        if (g_sdk == nullptr || g_sdk->ticket_result.defer_current_result == nullptr) return false;
        (void)g_sdk->ticket_result.defer_current_result();
        return true;
    };
    SaveTransitionTicketContext transition_context{};
    if (transition_write && attempt != 0) {
        if (!transition_ticket_context(p.request_sequence, p.kind, p.slot, &transition_context))
            return fail_transition("slot transition not confirmed");
        TransitionResult transition_result{};
        (void)save_transition_query(transition_context.token, &transition_result);
        if (transition_result.state == TransitionResultState::kInProgress) {
            return defer_transition();
        }
        if (transition_result.state != TransitionResultState::kReady ||
            transition_result.target_slot != p.slot ||
            transition_result.epoch <= transition_result.begin_epoch)
            return fail_transition("slot transition not confirmed");
    }
    // 联合保存协调器（唯一结算面）：kQolSaveWriteSave 绑定本命令自己的 tx——首次执行
    // 保存，后续 defer 只查询/推进该 tx；sidecar 正式提交确认前票据保持 pending，
    // 耗尽/degraded 给失败终态。kSave 的槽取运行时当前槽（保存写入的就是当前槽）。
    if (p.kind == contracts::kQolSaveWriteSave && save_coordinator_installed()) {
        int32_t slot = -1;
        if (g_port != nullptr && g_port->read.current_slot != nullptr) {
            (void)g_port->read.current_slot(&slot);
        }
        if (slot < 0 || slot > 2) {
            const std::string body = "{\"ok\":false,\"error\":\"save slot unavailable\"}";
            if (body.size() > capacity) return false;
            std::memcpy(out, body.data(), body.size());
            *out_size = static_cast<uint32_t>(body.size());
            return false;
        }
        std::string body;
        const auto settled = save_coordinator_settle(slot, attempt, g_frame.load(), &body);
        if (settled == qol::features::core_save::SaveSettle::kPending) {
            if (g_sdk != nullptr && g_sdk->ticket_result.defer_current_result != nullptr) {
                (void)g_sdk->ticket_result.defer_current_result();
                return true;
            }
            return false;
        }
        if (settled == qol::features::core_save::SaveSettle::kFailed && body.empty())
            body = "{\"ok\":false,\"error\":\"save failed\"}";
        if (!body.empty()) {
            if (body.size() > capacity) return false;
            std::memcpy(out, body.data(), body.size());
            *out_size = static_cast<uint32_t>(body.size());
            return settled == qol::features::core_save::SaveSettle::kCommitted;
        }
    }
    const uint64_t previous_request_sequence = g_active_save_request_sequence;
    g_active_save_request_sequence = transition_write ? p.request_sequence : 0;
    const QolResult result =
        g_save_write->read.write(p.kind, p.slot, p.class_idx, attempt, out, capacity, out_size);
    g_active_save_request_sequence = previous_request_sequence;
    if (result.code == kQolErrNotReady) {
        // 转场后世界态未提交：保持票据 pending，下一帧重试（不提前报告 done）。
        if (transition_write && attempt == 0 &&
            !transition_ticket_context(p.request_sequence, p.kind, p.slot, &transition_context))
            return fail_transition("slot transition not confirmed");
        if (defer_transition()) {
            return true;
        }
        return fail_transition("slot transition not confirmed");
    }
    // 业务失败时端口返回错误，但 out 已携带原错误体（与迁移前响应一致），票据置 failed。
    if (!contracts::succeeded(result) || *out_size == 0) {
        if (transition_write) clear_transition_ticket_context(p.request_sequence);
        return false;
    }

    // The ticket may complete only from its own token, never from global ready state.
    if (transition_write) {
        if (!transition_ticket_context(p.request_sequence, p.kind, p.slot, &transition_context))
            return fail_transition("slot transition not confirmed");
        TransitionResult transition_result{};
        (void)save_transition_query(transition_context.token, &transition_result);
        if (transition_result.state == TransitionResultState::kInProgress) {
            if (defer_transition()) return true;
            return fail_transition("slot transition not confirmed");
        }
        if (transition_result.state != TransitionResultState::kReady ||
            transition_result.target_slot != p.slot ||
            transition_result.epoch <= transition_result.begin_epoch)
            return fail_transition("slot transition not confirmed");
    }

    int32_t slot = p.slot;
    if (p.kind == contracts::kQolSaveWriteSave) {
        if (g_port == nullptr || g_port->read.current_slot == nullptr ||
            !contracts::succeeded(g_port->read.current_slot(&slot))) {
            return false;
        }
    }
    std::string body;
    switch (p.kind) {
        case contracts::kQolSaveWriteSave:
            body = "{\"ok\":true,\"slot\":" + std::to_string(slot) +
                   ",\"sidecar\":\"ensured\"}";
            break;
        case contracts::kQolSaveWriteEnterSlot:
            body = "{\"ok\":true,\"slot\":" + std::to_string(slot) +
                   ",\"entered\":true}";
            break;
        case contracts::kQolSaveWriteCreateSlot:
            body = "{\"ok\":true,\"slot\":" + std::to_string(slot) +
                   ",\"created\":true}";
            break;
        default:
            return false;
    }
    if (body.size() > capacity) return false;
    std::memcpy(out, body.data(), body.size());
    *out_size = static_cast<uint32_t>(body.size());
    if (transition_write) clear_transition_ticket_context(p.request_sequence);
    return true;
}

// 写命令投递：校验与迁移前 Kotlin 一致（slot ∈ [0,2] 否则 400 slot required (0-2)；
// create_slot 另需 class_idx 非负），通过后投递票据并立即返回 {"ok":true,"ticket":N}。
QolResult cs_save_write_command(contracts::HttpRes* response, SaveWriteRoute kind, int32_t slot,
                                int32_t class_idx) {
    if (kind == SaveWriteRoute::kEnterSlot || kind == SaveWriteRoute::kCreateSlot) {
        if (slot < 0 || slot > 2) return cs_http_error(response, 400, "slot required (0-2)");
    }
    if (kind == SaveWriteRoute::kCreateSlot && class_idx < 0) {
        return cs_http_error(response, 400, "class_idx required (0-5)");
    }
    if (g_sdk == nullptr || g_sdk->commands.post_typed == nullptr) {
        return cs_http_error(response, 503, "command port unavailable");
    }
    SaveWritePayload payload{};
    payload.kind = kind == SaveWriteRoute::kSave ? contracts::kQolSaveWriteSave
                   : kind == SaveWriteRoute::kEnterSlot ? contracts::kQolSaveWriteEnterSlot
                                                        : contracts::kQolSaveWriteCreateSlot;
    payload.slot = slot;
    payload.class_idx = class_idx;
    if (kind != SaveWriteRoute::kSave) {
        payload.request_sequence = g_next_save_request_sequence.fetch_add(1, std::memory_order_relaxed);
        if (payload.request_sequence == 0)
            return cs_http_error(response, 503, "command queue unavailable");
    }
    contracts::QolCommandTicket ticket = 0;
    if (!contracts::succeeded(g_sdk->commands.post_typed(kSaveWriteHandlerId, &payload,
                                                         sizeof(payload), &ticket)) ||
        ticket == 0) {
        return cs_http_error(response, 503, "command queue unavailable");
    }
    return cs_http_body(response, std::string("{\"ok\":true,\"ticket\":") +
                                        std::to_string(ticket) + "}");
}

QolResult cs_http_handler(const contracts::HttpReq* request, contracts::HttpRes* response,
                          void* ctx) {
    if (request == nullptr || response == nullptr || ctx == nullptr) {
        return cs_http_error(response, 400, "bad request");
    }
    const SaveWriteRoute kind = *static_cast<const SaveWriteRoute*>(ctx);
    const std::string body_text(
        request->body != nullptr ? reinterpret_cast<const char*>(request->body) : "",
        request->body_len);
    const char* body = body_text.c_str();
    int slot = -1;
    int class_idx = -1;
    switch (kind) {
        case SaveWriteRoute::kSave:
            return cs_save_write_command(response, kind, -1, -1);
        case SaveWriteRoute::kEnterSlot:
            (void)cs_extract_int(body, "slot", &slot);
            return cs_save_write_command(response, kind, slot, -1);
        case SaveWriteRoute::kCreateSlot:
            (void)cs_extract_int(body, "slot", &slot);
            (void)cs_extract_int(body, "class_idx", &class_idx);
            return cs_save_write_command(response, kind, slot, class_idx);
    }
    return cs_http_error(response, 400, "bad request");
}

const SaveWriteRoute kSaveWriteRouteKinds[] = {
    SaveWriteRoute::kSave,
    SaveWriteRoute::kEnterSlot,
    SaveWriteRoute::kCreateSlot,
};

const contracts::HttpQueryHandlerDescriptor kSaveWriteRoutes[] = {
    {contracts::HttpMethod::kPost, "/api/system/save", kPluginId, &cs_http_handler,
     const_cast<SaveWriteRoute*>(&kSaveWriteRouteKinds[0])},
    {contracts::HttpMethod::kPost, "/api/system/enter_slot", kPluginId, &cs_http_handler,
     const_cast<SaveWriteRoute*>(&kSaveWriteRouteKinds[1])},
    {contracts::HttpMethod::kPost, "/api/system/create_slot", kPluginId, &cs_http_handler,
     const_cast<SaveWriteRoute*>(&kSaveWriteRouteKinds[2])},
};

void plugin_deactivate(contracts::PluginContext& ctx);

// ---- 资源声明（P3c）：SAVE_Save 完整入口 + 新档边界 + 选角取消 ----

int32_t save_on_resource_ready_trampoline(const char* resource_id, void* backup);

const contracts::ResourceClaim kResources[] = {
    {"hook:SAVE_Save",
     "elf-export:SAVE_Save",
     contracts::ResourceMechanism::kEntryHook,
     contracts::ResourcePolicy::kPrivate,
     contracts::ResourceRequirement::kRequired,
     contracts::ResourceFailure::kFailClosed,
     true,
     "core.save",
     "SAVE_Save 是全部原版/自动保存的唯一写盘入口；接管后联合保存经协调器执行，"
     "读档期调用走 loading 旁路矩阵",
     nullptr,
     0,
     reinterpret_cast<void*>(&save_save_wrapper)},
    {"hook:SaveSlot_GoToNewGame",
     "elf-export:SaveSlot_GoToNewGame",
     contracts::ResourceMechanism::kEntryHook,
     contracts::ResourcePolicy::kPrivate,
     contracts::ResourceRequirement::kRequired,
     contracts::ResourceFailure::kFailClosed,
     true,
     "core.save",
     "新档边界前移：入口 begin transition（准入失败阻断删档与状态写），确认序按 "
     "selection token id 复用",
     nullptr,
     0,
     reinterpret_cast<void*>(&goto_new_game_wrapper)},
    {"hook:SelectCharacter_ButtonBackExe",
     "elf-export:SelectCharacter_ButtonBackExe",
     contracts::ResourceMechanism::kEntryHook,
     contracts::ResourcePolicy::kPrivate,
     contracts::ResourceRequirement::kRequired,
     contracts::ResourceFailure::kFailClosed,
     true,
     "core.save",
     "选角取消只释放 selection token/lease，不伪装恢复已删 dat",
     nullptr,
     0,
     reinterpret_cast<void*>(&select_char_back_wrapper)},
};

int plugin_install(contracts::PluginContext& ctx) {
    clear_all_transition_ticket_contexts();
    g_port = nullptr;
    g_save_write = nullptr;
    g_save_tx = nullptr;
    g_sdk = nullptr;
    g_game = nullptr;
    g_owner = 0;
    g_save_route_binding = 0;
    g_backup_save_save = nullptr;
    g_backup_goto_new_game = nullptr;
    g_backup_select_back = nullptr;
    g_logic_clock_task = 0;
    if (ctx.struct_size < contracts::PLUGIN_CONTEXT_PORTS_STRUCT_SIZE ||
        ctx.sdk_major != contracts::kHostSdkMajor || ctx.sdk == nullptr || ctx.ports == nullptr ||
        ctx.port_count != 3 || ctx.ports[0] == nullptr || ctx.ports[1] == nullptr ||
        ctx.ports[2] == nullptr) {
        return 1;
    }
    g_port = static_cast<const contracts::QolSavePort*>(ctx.ports[0]);
    if (!contracts::satisfies(g_port->abi_version,
                              {contracts::kQolPortSaveVersion.major,
                               contracts::kQolPortSaveVersion.minor}) ||
        g_port->read.slot_state == nullptr || g_port->read.load_data == nullptr ||
        g_port->read.is_ok == nullptr) {
        g_port = nullptr;
        return 1;
    }
    const auto* save_write = static_cast<const contracts::QolSaveWritePort*>(ctx.ports[1]);
    if (!contracts::satisfies(save_write->abi_version,
                              {contracts::kQolPortSaveWriteVersion.major,
                               contracts::kQolPortSaveWriteVersion.minor}) ||
        save_write->read.write == nullptr || ctx.sdk->abi.size < sizeof(contracts::QolHostSdk) ||
        ctx.sdk->abi.minor < 9 || ctx.sdk->ticket_result.register_typed_result_handler == nullptr) {
        g_port = nullptr;
        return 1;
    }
    g_save_write = save_write;
    // 保存事务 Adapter（P3b-2 面）：受控原版转换/保存/evidence 与 loading 旁路探针。
    g_save_tx = static_cast<const contracts::QolSaveTransactionPort*>(ctx.ports[2]);
    if (!contracts::satisfies(QolVersion{g_save_tx->abi.major, g_save_tx->abi.minor},
                              {contracts::kQolPortSaveTransactionVersion.major,
                               contracts::kQolPortSaveTransactionVersion.minor}) ||
        g_save_tx->read_state == nullptr || g_save_tx->bind_backup == nullptr ||
        g_save_tx->call_original_save == nullptr || g_save_tx->read_evidence == nullptr) {
        g_save_tx = nullptr;
        g_save_write = nullptr;
        g_port = nullptr;
        return 1;
    }
    g_sdk = ctx.sdk;
    g_game = ctx.game;
    g_owner = ctx.owner_token;
    if (!contracts::succeeded(initialize_slot_data(ctx))) {
        g_save_tx = nullptr;
        g_save_write = nullptr;
        g_sdk = nullptr;
        g_port = nullptr;
        return 1;
    }
    // 转换状态机与联合协调器（P3b-3 面）：本批起生产装配；缝注入后确认序的 journal
    // 分派与槽健康准入归协调器。
    if (!contracts::succeeded(save_transition_initialize(
            g_owner, ctx.sdk->owner_verify_token, ctx.sdk->owner_store, ctx.sdk->slot_operation,
            ctx.game->thread.is_main_thread)) ||
        !contracts::succeeded(save_coordinator_initialize(
            g_owner, ctx.sdk->owner_verify_token, ctx.sdk->owner_store, ctx.sdk->slot_operation,
            g_save_tx, ctx.game->thread.is_main_thread))) {
        save_transition_shutdown();
        save_coordinator_shutdown();
        slot_data_set_available(false);
        g_save_tx = nullptr;
        g_save_write = nullptr;
        g_sdk = nullptr;
        g_port = nullptr;
        return 1;
    }
    save_transition_set_transition_gate(&save_transition_gate_admits, nullptr);
    save_transition_set_journal_verdict(&save_coordinator_journal_verdict, nullptr);
    // save_route 领域回调注册：SAVE_Save wrapper / begin thunk 派发的唯一路由。
    if (ctx.sdk->save_route == nullptr || ctx.sdk->save_route->bind == nullptr) {
        save_transition_shutdown();
        save_coordinator_shutdown();
        slot_data_set_available(false);
        g_save_tx = nullptr;
        g_save_write = nullptr;
        g_sdk = nullptr;
        g_port = nullptr;
        return 1;
    }
    const contracts::QolSaveOwnerCallbacks callbacks{
        {sizeof(contracts::QolSaveOwnerCallbacks), 1, 0}, nullptr, &cs_native_call,
        &cs_render_ready, &cs_prepare_stop};
    if (!contracts::succeeded(ctx.sdk->save_route->bind(ctx.sdk->save_route->host_ctx, g_owner,
                                                        &callbacks, &g_save_route_binding))) {
        g_save_route_binding = 0;
        save_transition_shutdown();
        save_coordinator_shutdown();
        slot_data_set_available(false);
        g_save_tx = nullptr;
        g_save_write = nullptr;
        g_sdk = nullptr;
        g_port = nullptr;
        return 1;
    }
    if (!contracts::succeeded(ctx.sdk->ticket_result.register_typed_result_handler(
            kSaveWriteHandlerId, &save_write_result_handler, nullptr))) {
        if (ctx.sdk->save_route->unbind != nullptr && g_save_route_binding != 0) {
            (void)ctx.sdk->save_route->unbind(ctx.sdk->save_route->host_ctx, g_owner,
                                              g_save_route_binding);
        }
        g_save_route_binding = 0;
        save_transition_shutdown();
        save_coordinator_shutdown();
        slot_data_set_available(false);
        g_save_tx = nullptr;
        g_save_write = nullptr;
        g_sdk = nullptr;
        g_port = nullptr;
        return 1;
    }
    if (ctx.sdk->scheduler.add != nullptr && g_slot_data_frame_task == 0) {
        const auto scheduled = ctx.sdk->scheduler.add(contracts::kQolRuntimeFramePointRenderPre,
            &slot_data_frame, nullptr, 1, 0, &g_slot_data_frame_task);
        if (!contracts::succeeded(scheduled)) {
            (void)ctx.sdk->ticket_result.unregister_typed_result_handler(kSaveWriteHandlerId);
            if (ctx.sdk->save_route->unbind != nullptr && g_save_route_binding != 0) {
                (void)ctx.sdk->save_route->unbind(ctx.sdk->save_route->host_ctx, g_owner,
                                                  g_save_route_binding);
            }
            g_save_route_binding = 0;
            save_transition_shutdown();
            save_coordinator_shutdown();
            slot_data_set_available(false);
            g_save_tx = nullptr;
            g_save_write = nullptr;
            g_sdk = nullptr;
            g_port = nullptr;
            return 1;
        }
    }
    // LogicPre is required: without it a failed load can leave a transition pending forever.
    if (ctx.sdk->scheduler.add == nullptr || g_logic_clock_task != 0) {
        plugin_deactivate(ctx);
        return 1;
    }
    const auto logic_scheduled = ctx.sdk->scheduler.add(
        contracts::kQolRuntimeFramePointLogicPre, &logic_clock_frame, nullptr, 1, 0,
        &g_logic_clock_task);
    if (!contracts::succeeded(logic_scheduled) || g_logic_clock_task == 0) {
        g_logic_clock_task = 0;
        plugin_deactivate(ctx);
        return 1;
    }
    log_message(ctx.sdk, "[core.save] provider install: facts + save writes + save route + hooks");
    return 0;
}
void plugin_deactivate(contracts::PluginContext& ctx) {
    // §9 停用顺序：先关准入（拒绝新请求与新转换）→ 摘 save_route 派发 → 收敛未决事务
    //（degraded + 释放凭据）→ 关转换状态机 → 摘帧任务/票据处理器。
    slot_data_set_available(false);
    if (g_sdk != nullptr && g_sdk->save_route != nullptr && g_sdk->save_route->unbind != nullptr &&
        g_save_route_binding != 0) {
        (void)g_sdk->save_route->unbind(g_sdk->save_route->host_ctx, g_owner, g_save_route_binding);
    }
    g_save_route_binding = 0;
    save_coordinator_shutdown();
    save_transition_shutdown();
    clear_all_transition_ticket_contexts();
    if (g_slot_data_frame_task != 0 && g_sdk != nullptr && g_sdk->scheduler.remove != nullptr) {
        (void)g_sdk->scheduler.remove(g_slot_data_frame_task);
        g_slot_data_frame_task = 0;
    }
    if (g_logic_clock_task != 0 && g_sdk != nullptr && g_sdk->scheduler.remove != nullptr) {
        (void)g_sdk->scheduler.remove(g_logic_clock_task);
        g_logic_clock_task = 0;
    }
    // 结果处理器为自注册（不在 descriptor），必须显式注销。
    if (g_sdk != nullptr && g_sdk->ticket_result.unregister_typed_result_handler != nullptr) {
        (void)g_sdk->ticket_result.unregister_typed_result_handler(kSaveWriteHandlerId);
    }
    log_message(ctx.sdk, "[core.save] provider deactivate");
    g_save_tx = nullptr;
    g_save_write = nullptr;
    g_sdk = nullptr;
    g_game = nullptr;
    g_port = nullptr;
}

void plugin_on_enabled(contracts::PluginContext& ctx) {
    slot_data_set_available(contracts::succeeded(initialize_slot_data(ctx)));
    log_message(ctx.sdk, "[core.save] provider enabled: capability published");
}

void plugin_on_disabled(contracts::PluginContext& ctx) {
    slot_data_set_available(false);
    log_message(ctx.sdk, "[core.save] provider disabled: capability withdrawn");
}

void plugin_fill_status(contracts::PluginStatusOut& out) {
    out.enabled = g_port != nullptr;
    out.error = nullptr;
}

// 受限回调（§3.3）：SAVE_Save 的 backup 经 Adapter 常数时间绑定；其余两个入口只缓存
// trampoline。返回非 0 → 宿主按 resource_ready_failed 撤回该资源。
int32_t save_on_resource_ready_impl(const char* resource_id, void* backup) {
    if (resource_id == nullptr) return 1;
    if (std::strcmp(resource_id, "hook:SAVE_Save") == 0) {
        if (backup == nullptr || g_save_tx == nullptr || g_save_tx->bind_backup == nullptr)
            return 1;
        return contracts::succeeded(g_save_tx->bind_backup(
                   g_save_tx->adapter_ctx, contracts::kQolSaveBackupEntrySaveSave, backup))
                   ? 0
                   : 1;
    }
    if (std::strcmp(resource_id, "hook:SaveSlot_GoToNewGame") == 0) {
        if (backup == nullptr) return 1;
        g_backup_goto_new_game = reinterpret_cast<GoToNewGameFn>(backup);
        return 0;
    }
    if (std::strcmp(resource_id, "hook:SelectCharacter_ButtonBackExe") == 0) {
        if (backup == nullptr) return 1;
        g_backup_select_back = reinterpret_cast<SelectCharBackFn>(backup);
        return 0;
    }
    return 1;
}

int32_t save_on_resource_ready_trampoline(contracts::PluginContext& /*ctx*/,
                                          const char* resource_id, void* backup) {
    return save_on_resource_ready_impl(resource_id, backup);
}

const contracts::PluginDescriptor kDescriptor = {
    kPluginId, kPluginVersion,
    0, nullptr,
    0, nullptr,
    2, kProvides,
    static_cast<uint32_t>(sizeof(kResources) / sizeof(kResources[0])), kResources,
    0, nullptr,
    &plugin_install, &plugin_deactivate, &plugin_on_enabled, &plugin_on_disabled,
    &plugin_fill_status, &save_on_resource_ready_trampoline,
    kPorts, 3,
    kSaveWriteRoutes, 3,
};

}  // namespace

const contracts::PluginDescriptor& plugin_descriptor() { return kDescriptor; }

}  // namespace qol::features::core_save
