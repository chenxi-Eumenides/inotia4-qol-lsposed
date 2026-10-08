#include "game/access/save_transaction.h"

#include <sys/stat.h>
#include <atomic>
#include <cerrno>
#include <cstring>
#include <mutex>
#include <string>

#include "contracts/save_route.h"
#include "game/access/save_file_evidence.h"

namespace qol::game {
namespace {

constexpr uint32_t kArm64BlOpcode = 0x25u << 26;  // BL：100101
constexpr size_t kNameBufferBytes = 256;

std::mutex g_invocation_mtx;
struct SaveInvocation {
    uint64_t id;
    uint32_t kind;
    int32_t target_slot;
};
SaveInvocation g_invocation{};
uint64_t g_next_invocation = 1;

std::mutex g_transition_mtx;
struct PendingTransition {
    uint64_t transition;
    uint64_t invocation;
    int32_t target_slot;
    bool present;
    bool ready;
};
PendingTransition g_pending_transition{};
std::atomic<bool> g_joint_save_in_flight{false};

SaveTransactionGate g_gate{};
char g_platform_dir[512] = {};
void (*g_get_name_fn)(int32_t, char*) = nullptr;
int (*g_current_slot_fn)() = nullptr;
int (*g_resume_fn)(int32_t) = nullptr;
void (*g_new_game_fn)(int32_t) = nullptr;

// 已绑定的 framework trampoline；常数时间绑定，仅 resource-ready 装配回调写入。
void* g_framework_backup = nullptr;

QolResult fail(int32_t code, const char* text) {
    return contracts::error(code, text, static_cast<uint32_t>(std::strlen(text)));
}

bool gate_admits(const contracts::QolSlotOperationLease* lease, int32_t slot) {
    if (g_gate.operation_admits == nullptr || lease == nullptr) return false;
    return g_gate.operation_admits(g_gate.ctx, lease->owner, lease, slot);
}

// 联合保存事务在原版调用窗口的标记：loading 旁路矩阵据此拒绝嵌套（P3c）。
struct JointSaveInFlightGuard {
    const bool previous = g_joint_save_in_flight.load(std::memory_order_acquire);
    JointSaveInFlightGuard() { g_joint_save_in_flight.store(true, std::memory_order_release); }
    ~JointSaveInFlightGuard() {
        g_joint_save_in_flight.store(previous, std::memory_order_release);
    }
};

}  // namespace

SaveBypassPlan save_bypass_scan(uintptr_t entry, uintptr_t span, uintptr_t save_entry,
                                SaveBypassReadFn read, void* read_ctx) {
    SaveBypassPlan plan{0, save_entry, false};
    if (entry == 0 || save_entry == 0 || read == nullptr) return plan;
    if (span == 0 || (span & 0x3u) != 0 || span > 0x1000) return plan;
    uintptr_t found = 0;
    uint32_t hits = 0;
    for (uintptr_t offset = 0; offset < span; offset += 4) {
        const uintptr_t site = entry + offset;
        uint32_t word = 0;
        if (!read(read_ctx, site, &word)) break;
        if ((word & 0xFC000000u) != kArm64BlOpcode) continue;
        int64_t immediate = static_cast<int64_t>(word & 0x03FFFFFFu);
        if ((immediate & 0x02000000) != 0) immediate -= 0x04000000;
        if (site + static_cast<uintptr_t>(immediate * 4) == save_entry) {
            found = site;
            ++hits;
        }
    }
    // 唯一命中才建立合法返回点；零命中或多命中均为变体差异或解析异常，拒绝建立。
    if (hits == 1) {
        plan.callsite = found;
        plan.valid = true;
    }
    return plan;
}

uint64_t save_invocation_begin(uint32_t kind, int32_t target_slot) {
    std::lock_guard<std::mutex> lock(g_invocation_mtx);
    g_invocation = SaveInvocation{g_next_invocation, kind, target_slot};
    return g_next_invocation++;
}

bool save_invocation_valid(uint64_t invocation, uint32_t kind, int32_t target_slot) {
    if (invocation == 0) return false;
    std::lock_guard<std::mutex> lock(g_invocation_mtx);
    return g_invocation.id == invocation && g_invocation.kind == kind &&
           g_invocation.target_slot == target_slot;
}

bool save_invocation_current_matches(uint32_t kind, int32_t target_slot) {
    std::lock_guard<std::mutex> lock(g_invocation_mtx);
    return g_invocation.id != 0 && g_invocation.kind == kind &&
           g_invocation.target_slot == target_slot;
}

void save_invocation_end(uint64_t invocation) {
    if (invocation == 0) return;
    std::lock_guard<std::mutex> lock(g_invocation_mtx);
    // 仅当仍未被更新的 invocation 覆盖时清除；保证「仅当前回调有效」。
    if (g_invocation.id == invocation) g_invocation = SaveInvocation{};
}

QolResult save_admit_loading_bypass(const SaveLoadingBypassInput& input) {
    // session ready 时的 SAVE_Save 是普通保存路径，不归 loading 旁路管辖。
    if (input.session_ready) return contracts::ok();
    if (!input.on_game_main_thread || !input.invocation_verified ||
        !input.has_unready_load_transition || input.transition_target != input.current_slot ||
        !input.lease_covers_slot || input.joint_save_in_flight)
        return fail(kQolErrBlocked, "loading bypass not admitted");
    return contracts::ok();
}

QolResult save_loading_bypass_admit(const contracts::QolSlotOperationLease* lease, int32_t slot) {
    if (lease == nullptr || lease->size < sizeof(*lease) || slot < 0 || slot > 2)
        return fail(kQolErrInvalidArgument, "invalid bypass probe");
    int32_t transition_target = -1;
    const bool has_unready = save_has_unready_load_transition(&transition_target);
    int32_t current = -1;
    if (g_current_slot_fn != nullptr) current = g_current_slot_fn();
    SaveLoadingBypassInput input{};
    // 主线程由调用方（core.save wrapper 的 is_main 缝）前置判定；此处只对 Game 层
    // 可观测事实执行矩阵。session_ready 恒 false：探针仅在 session 非 ready 时可达。
    input.on_game_main_thread = true;
    input.invocation_verified = save_invocation_current_matches(
        static_cast<uint32_t>(contracts::QolSaveCallKind::kResume), slot);
    input.has_unready_load_transition = has_unready;
    input.transition_target = transition_target;
    input.current_slot = current;
    input.lease_covers_slot = gate_admits(lease, slot);
    input.joint_save_in_flight = g_joint_save_in_flight.load(std::memory_order_acquire);
    input.session_ready = false;
    return save_admit_loading_bypass(input);
}

void save_set_pending_transition(uint64_t transition, int32_t target_slot, bool ready) {
    std::lock_guard<std::mutex> lock(g_transition_mtx);
    g_pending_transition = PendingTransition{transition, 0, target_slot, transition != 0, ready};
}

void save_clear_pending_transition() {
    std::lock_guard<std::mutex> lock(g_transition_mtx);
    g_pending_transition = PendingTransition{};
}

bool save_has_unready_load_transition(int32_t* out_target) {
    std::lock_guard<std::mutex> lock(g_transition_mtx);
    if (out_target != nullptr) *out_target = g_pending_transition.target_slot;
    return g_pending_transition.present && !g_pending_transition.ready;
}

void save_set_joint_save_in_flight(bool in_flight) {
    g_joint_save_in_flight.store(in_flight, std::memory_order_release);
}

void save_transaction_bind_gate(const SaveTransactionGate* gate) {
    g_gate = gate != nullptr ? *gate : SaveTransactionGate{};
}

void save_transaction_set_platform_dir(const char* root) {
    if (root == nullptr) {
        g_platform_dir[0] = '\0';
        return;
    }
    const size_t length = std::strlen(root);
    if (length == 0 || length >= sizeof(g_platform_dir)) return;
    std::memcpy(g_platform_dir, root, length + 1);
}

void save_transaction_set_get_name_fn(void (*fn)(int32_t, char*)) { g_get_name_fn = fn; }

void save_transaction_set_current_slot_fn(int (*fn)()) { g_current_slot_fn = fn; }
void save_transaction_set_resume_fn(int (*fn)(int32_t)) { g_resume_fn = fn; }
void save_transaction_set_new_game_fn(void (*fn)(int32_t)) { g_new_game_fn = fn; }

uint64_t save_load_bypass_begin(uint64_t transition, int32_t target_slot, uint32_t kind) {
    if (transition == 0 || target_slot < 0 || target_slot > 2) return 0;
    {
        std::lock_guard<std::mutex> lock(g_transition_mtx);
        if (g_pending_transition.present) return 0;
        g_pending_transition = PendingTransition{transition, 0, target_slot, true, false};
    }
    const uint64_t invocation = save_invocation_begin(kind, target_slot);
    if (invocation != 0) {
        std::lock_guard<std::mutex> lock(g_transition_mtx);
        if (g_pending_transition.transition == transition)
            g_pending_transition.invocation = invocation;
    }
    if (invocation == 0) {
        std::lock_guard<std::mutex> lock(g_transition_mtx);
        if (g_pending_transition.transition == transition)
            g_pending_transition = PendingTransition{};
    }
    return invocation;
}

void save_load_bypass_end(uint64_t invocation) {
    if (invocation == 0) return;
    {
        std::lock_guard<std::mutex> lock(g_invocation_mtx);
        if (g_invocation.id != invocation) return;
    }
    {
        std::lock_guard<std::mutex> lock(g_transition_mtx);
        if (g_pending_transition.present && !g_pending_transition.ready &&
            g_pending_transition.invocation == invocation)
            g_pending_transition = PendingTransition{};
    }
    save_invocation_end(invocation);
}

QolResult save_transaction_bind_backup(uint32_t entry_kind, void* framework_backup) {
    (void)entry_kind;
    if (framework_backup == nullptr) return fail(kQolErrInvalidArgument, "invalid backup");
    // resource-ready 装配回调语义：常数时间绑定，不触碰游戏状态。
    g_framework_backup = framework_backup;
    return contracts::ok();
}

QolResult save_transaction_call_original_save(const contracts::QolSlotOperationLease* lease,
                                              int32_t expected_slot,
                                              contracts::QolSaveOriginalResult* out) {
    if (out == nullptr || lease == nullptr || lease->size < sizeof(*lease) ||
        expected_slot < 0 || expected_slot > 2)
        return fail(kQolErrInvalidArgument, "invalid original save call");
    out->size = sizeof(*out);
    out->invoked = 0;
    out->result_valid = 0;
    out->original_result = 0;
    // 未绑定 trampoline 即当前生产态：NotReady，不执行原版，不伪造结果。
    if (g_framework_backup == nullptr) return fail(kQolErrNotReady, "backup not bound");
    if (!gate_admits(lease, expected_slot))
        return fail(kQolErrBlocked, "operation lease does not cover slot");
    // 槽漂移防御：原版保存写入的是运行时当前槽，与凭据期望不一致时拒绝。
    // current slot 查询经装配期注入缝（生产指向 save_current_slot）。
    if (g_current_slot_fn == nullptr) return fail(kQolErrNotReady, "current slot unavailable");
    const int current = g_current_slot_fn();
    if (current != expected_slot) return fail(kQolErrBusy, "current slot changed");
    // 联合保存事务执行窗口：loading 旁路矩阵据此拒绝嵌套（P3c 生产接线）。
    const JointSaveInFlightGuard in_flight_guard;
    // 直接调用 Loader 交还的 trampoline；跳板跟随（monster 变体 B 跳板）由交还方
    // 处理，Adapter 不从 +4 自行绕过，也不直调可能被 hook 的入口以防递归。
    using BackupFn = int (*)();
    const int original = reinterpret_cast<BackupFn>(g_framework_backup)();
    out->invoked = 1;
    out->result_valid = 1;
    out->original_result = original;
    return contracts::ok();
}

QolResult save_transaction_call_transition(const contracts::QolSlotOperationLease* lease,
                                           const contracts::QolSaveTransitionCall* call,
                                           contracts::QolSaveOriginalResult* out) {
    if (out == nullptr || call == nullptr || call->size < sizeof(*call) || lease == nullptr ||
        lease->size < sizeof(*lease))
        return fail(kQolErrInvalidArgument, "invalid transition call");
    out->size = sizeof(*out);
    out->invoked = 0;
    out->result_valid = 0;
    out->original_result = 0;
    if (!gate_admits(lease, call->target_slot))
        return fail(kQolErrBlocked, "operation lease does not cover slot");
    const auto kind = static_cast<contracts::QolSaveTransitionKind>(call->kind);
    if (kind == contracts::QolSaveTransitionKind::kResume) {
        if (g_resume_fn == nullptr) return fail(kQolErrNotReady, "resume entry unavailable");
        const uint64_t bypass = save_load_bypass_begin(
            call->transition, call->target_slot,
            static_cast<uint32_t>(contracts::QolSaveCallKind::kResume));
        if (bypass == 0) return fail(kQolErrBusy, "transition invocation already active");
        const int original = g_resume_fn(call->target_slot);
        save_load_bypass_end(bypass);
        out->invoked = 1;
        out->result_valid = 1;
        out->original_result = original;
        return original != 0 ? contracts::ok() : fail(kQolErrInternal, "resume failed");
    }
    if (kind == contracts::QolSaveTransitionKind::kBeginNew) {
        return fail(kQolErrUnsupported, "begin new transition unsupported");
    }
    if (kind == contracts::QolSaveTransitionKind::kConfirmNew) {
        return fail(kQolErrUnsupported, "confirm new transition unsupported");
    }
    return fail(kQolErrUnsupported, "transition kind unsupported");
}

bool save_transaction_dat_relative_name_valid(const char* name) {
    if (name == nullptr) return false;
    const size_t length = std::strlen(name);
    if (length == 0 || length >= kNameBufferBytes) return false;
    if (name[0] == '.') return false;  // 隐藏/相对导航组件
    for (size_t i = 0; i < length; ++i) {
        const char c = name[i];
        const bool allowed = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                             (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-';
        if (!allowed) return false;
    }
    return std::strstr(name, "..") == nullptr;
}

QolResult save_transaction_read_evidence(const contracts::QolSlotOperationLease* lease,
                                         int32_t slot, contracts::QolSaveFileEvidence* out) {
    if (out == nullptr || lease == nullptr || lease->size < sizeof(*lease))
        return fail(kQolErrInvalidArgument, "invalid evidence request");
    if (slot < 0 || slot > 2) return fail(kQolErrInvalidArgument, "invalid slot");
    if (g_platform_dir[0] == '\0' || g_get_name_fn == nullptr)
        return fail(kQolErrNotReady, "platform path context unavailable");
    if (!gate_admits(lease, slot))
        return fail(kQolErrBlocked, "operation lease does not cover slot");
    char name[kNameBufferBytes] = {};
    g_get_name_fn(slot, name);
    name[kNameBufferBytes - 1] = '\0';
    if (!save_transaction_dat_relative_name_valid(name))
        return fail(kQolErrInvalidArgument, "unreliable save file name");
    std::string path(g_platform_dir);
    path += "/";
    path += name;
    struct stat info{};
    if (::stat(path.c_str(), &info) != 0)
        return fail(errno == ENOENT ? kQolErrNotFound : kQolErrInternal, "dat stat failed");
    if (!read_save_file_evidence(path.c_str(), out)) return fail(kQolErrInternal, "evidence read failed");
    // 路径只在本函数内部组合与使用，不返回给插件。
    return contracts::ok();
}

void save_transaction_reset_for_test() {
    std::lock_guard<std::mutex> lock(g_invocation_mtx);
    g_invocation = SaveInvocation{};
    g_next_invocation = 1;
    std::lock_guard<std::mutex> transition_lock(g_transition_mtx);
    g_pending_transition = PendingTransition{};
    g_joint_save_in_flight.store(false, std::memory_order_release);
    g_gate = SaveTransactionGate{};
    g_platform_dir[0] = '\0';
    g_get_name_fn = nullptr;
    g_current_slot_fn = nullptr;
    g_resume_fn = nullptr;
    g_new_game_fn = nullptr;
    g_framework_backup = nullptr;
}

}  // namespace qol::game
