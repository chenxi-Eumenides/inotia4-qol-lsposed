#include "hooks/transaction.h"

// 资源事务引擎实现（见 hooks/transaction.h 的接口契约与 ARCHITECTURE.md §4.4）。
// 固定容量静态数组：登记表上限 128 条，事务资源数不会超过它；终态记录保留最近 16 个。

#include <cstring>

#include "hooks/registry.h"

namespace qol::hooks {
namespace {

// 登记表容量（hooks/registry.cpp 同值）；事务资源列表按此设置上限。
constexpr uint32_t kMaxResources = 128;
// 终态事务保留条数：环形覆盖最旧，淘汰后查询返回 false。
constexpr uint32_t kMaxTerminal = 16;
// 并发未终态事务槽位数（每插件至多一个）。
constexpr uint32_t kMaxActive = 32;
constexpr uint32_t kDefaultBudget = 120;

struct TxEntry {
    const contracts::ResourceClaim* claim = nullptr;
    bool installed = false;      // 本事务内安装成功（安装回滚用）
    uint32_t waited_frames = 0;  // 还原等待该资源已消耗帧数
};

struct Tx {
    bool in_use = false;
    uint64_t id = 0;
    const char* plugin_id = nullptr;
    TxKind kind = TxKind::kInstall;
    TxState state = TxState::kRunning;
    uint32_t step = 0;    // 已完成资源步骤数
    uint32_t total = 0;   // 该插件资源总数
    uint32_t frames = 0;  // 已消耗帧数
    uint32_t budget = kDefaultBudget;
    const char* failed_resource_id = nullptr;
    const char* error = nullptr;
    TxEntry entries[kMaxResources];
};

struct TerminalRecord {
    bool used = false;
    uint64_t id = 0;
    TxStatus status;
};

Tx g_active[kMaxActive];
TerminalRecord g_terminal[kMaxTerminal];
uint32_t g_terminal_next = 0;  // 环形写入位置
TxDeps g_deps{};
bool g_deps_set = false;
uint64_t g_next_id = 1;  // 0 保留为「无效事务 id」

bool is_empty(const char* text) {
    return text == nullptr || text[0] == '\0';
}

uint32_t budget_of(const contracts::ResourceClaim& claim) {
    return claim.step_budget == 0 ? kDefaultBudget : claim.step_budget;
}

Tx* find_active(uint64_t tx_id) {
    if (tx_id == 0) {
        return nullptr;
    }
    for (Tx& tx : g_active) {
        if (tx.in_use && tx.id == tx_id) {
            return &tx;
        }
    }
    return nullptr;
}

Tx* find_active_by_plugin(const char* plugin_id) {
    for (Tx& tx : g_active) {
        if (tx.in_use && tx.plugin_id != nullptr && std::strcmp(tx.plugin_id, plugin_id) == 0) {
            return &tx;
        }
    }
    return nullptr;
}

void fill_status(const Tx& tx, TxStatus* out) {
    out->kind = tx.kind;
    out->state = tx.state;
    out->step = tx.step;
    out->total = tx.total;
    out->frames = tx.frames;
    out->budget = tx.budget;
    out->failed_resource_id = tx.failed_resource_id;
    out->error = tx.error;
}

// 令事务进入终态并写入最近终态环；终态后事务从 active 表移出。
void finalize_terminal(Tx& tx) {
    TerminalRecord& record = g_terminal[g_terminal_next];
    record.used = true;
    record.id = tx.id;
    fill_status(tx, &record.status);
    g_terminal_next = (g_terminal_next + 1) % kMaxTerminal;
    tx.in_use = false;
}

// 释放一条非入口机制资源（机制分派在宿主）；true = 成功。
bool release_non_entry(const contracts::ResourceClaim& claim, const char** out_error) {
    if (g_deps.release_other == nullptr) {
        *out_error = "mechanism unsupported";
        return false;
    }
    const TxDeps::MechanismOutcome outcome = g_deps.release_other(claim, 0);
    if (outcome.ok != 0) {
        *out_error = outcome.error != nullptr ? outcome.error : "mechanism release failed";
        return false;
    }
    return true;
}

// 安装失败后按逆序释放本事务已安装的子集：
// - resumable==false → 不调释放原语，保留占用并隔离；
// - 释放失败 → tombstone + 隔离；成功 → 入口 hook 由 release 内部置 kFree，
//   其它机制由本层置 kFree 并清运行期记录。
void rollback_install(Tx& tx) {
    for (uint32_t i = tx.step; i-- > 0;) {
        TxEntry& entry = tx.entries[i];
        if (!entry.installed) {
            continue;
        }
        const contracts::ResourceClaim& claim = *entry.claim;
        if (!claim.resumable) {
            set_state(claim.id, contracts::ResourceState::kTombstone);
            isolated_set(claim.id, true);
            continue;
        }
        if (claim.mechanism != contracts::ResourceMechanism::kEntryHook) {
            const char* error = nullptr;
            if (!release_non_entry(claim, &error)) {
                set_state(claim.id, contracts::ResourceState::kTombstone);
                isolated_set(claim.id, true);
            } else {
                set_state(claim.id, contracts::ResourceState::kFree);
                runtime_clear(claim.id);
            }
            continue;
        }
        ResourceRuntime runtime{};
        const uintptr_t entry_address = runtime_of(claim.id, &runtime) ? runtime.resolved_entry : 0;
        const ReleaseOutcome outcome = release_claim_entry_hook(claim, entry_address, g_deps.unhook_fn);
        if (outcome.ok != 0) {
            set_state(claim.id, contracts::ResourceState::kTombstone);
            isolated_set(claim.id, true);
        }
    }
}

bool fail_install(Tx& tx, const char* resource_id, const char* error, TxStep* out) {
    tx.failed_resource_id = resource_id;
    tx.error = error;
    rollback_install(tx);
    tx.state = TxState::kFailed;
    out->resource_id = resource_id;
    out->ok = 1;
    out->backup = nullptr;
    out->finished = true;
    finalize_terminal(tx);
    return false;
}

bool fail_restore(Tx& tx, const char* resource_id, const char* error, TxStep* out) {
    tx.failed_resource_id = resource_id;
    tx.error = error;
    tx.state = TxState::kFailed;
    out->resource_id = resource_id;
    out->ok = 1;
    out->backup = nullptr;
    out->finished = true;
    finalize_terminal(tx);
    return false;
}

// 非入口机制安装步：call patch 不支持空转（§4.2 规则 6），gate 关不安装、登记保持 free。
bool install_other_step(Tx& tx, const contracts::ResourceClaim& claim, uint32_t index,
                        TxStep* out) {
    bool want = true;
    if (g_deps.gate_on != nullptr && claim.gate != nullptr) {
        want = g_deps.gate_on(claim.gate);
    }
    if (!want) {
        ++tx.step;
        out->resource_id = claim.id;
        out->ok = 0;
        out->finished = tx.step >= tx.total;
        if (out->finished) {
            tx.state = TxState::kSucceeded;
            finalize_terminal(tx);
        }
        return !out->finished;
    }
    if (g_deps.install_other == nullptr) {
        return fail_install(tx, claim.id, "mechanism unsupported", out);
    }
    const TxDeps::MechanismOutcome outcome = g_deps.install_other(claim, 0);
    if (outcome.ok != 0) {
        return fail_install(tx, claim.id,
                            outcome.error != nullptr ? outcome.error : "mechanism install failed",
                            out);
    }
    set_state(claim.id, contracts::ResourceState::kLive);
    runtime_set(claim.id, 0, nullptr);
    tx.entries[index].installed = true;
    ++tx.step;
    out->resource_id = claim.id;
    out->ok = 0;
    out->backup = nullptr;
    out->finished = tx.step >= tx.total;
    if (out->finished) {
        tx.state = TxState::kSucceeded;
        finalize_terminal(tx);
    }
    return !out->finished;
}

// 非入口机制还原步：无 wrapper 故无在途等待，直接按 resumable 决定释放或保留占用。
bool restore_other_step(Tx& tx, const contracts::ResourceClaim& claim, TxStep* out) {
    if (!claim.resumable) {
        set_state(claim.id, contracts::ResourceState::kTombstone);
        isolated_set(claim.id, true);
        return fail_restore(tx, claim.id, "not resumable", out);
    }
    const char* error = nullptr;
    if (!release_non_entry(claim, &error)) {
        set_state(claim.id, contracts::ResourceState::kTombstone);
        isolated_set(claim.id, true);
        return fail_restore(tx, claim.id, error, out);
    }
    set_state(claim.id, contracts::ResourceState::kFree);
    runtime_clear(claim.id);
    ++tx.step;
    out->resource_id = claim.id;
    out->ok = 0;
    out->finished = tx.step >= tx.total;
    if (out->finished) {
        tx.state = TxState::kSucceeded;
        finalize_terminal(tx);
    }
    return !out->finished;
}

bool install_step(Tx& tx, TxStep* out) {
    if (tx.step >= tx.total) {
        tx.state = TxState::kSucceeded;
        out->finished = true;
        finalize_terminal(tx);
        return false;
    }
    const uint32_t index = tx.step;
    const contracts::ResourceClaim& claim = *tx.entries[index].claim;
    tx.budget = budget_of(claim);

    const contracts::ResourceState state = state_of(claim.id);
    if (state == contracts::ResourceState::kLive || state == contracts::ResourceState::kDisabled) {
        // 幂等跳过：资源已安装，本步无副作用。
        ++tx.step;
        out->resource_id = claim.id;
        out->ok = 0;
        out->finished = tx.step >= tx.total;
        if (out->finished) {
            tx.state = TxState::kSucceeded;
            finalize_terminal(tx);
        }
        return !out->finished;
    }
    if (state != contracts::ResourceState::kFree) {
        return fail_install(tx, claim.id, "resource not free", out);
    }
    // 机制分派：入口 hook 走既有路径；其它机制交宿主注入的 install_other。
    if (claim.mechanism != contracts::ResourceMechanism::kEntryHook) {
        return install_other_step(tx, claim, index, out);
    }

    bool followed = false;
    const uintptr_t entry = g_deps.resolve_entry(claim.symbol, &followed);
    if (entry == 0) {
        return fail_install(tx, claim.id, "symbol not resolved", out);
    }

    // gate 为 nullptr 视为开；gate_on 未注入时一切 gate 视为开。
    bool want = true;
    if (g_deps.gate_on != nullptr && claim.gate != nullptr) {
        want = g_deps.gate_on(claim.gate);
    }

    void* backup = nullptr;
    const int rc = g_deps.hook_fn(reinterpret_cast<void*>(entry), claim.handler, &backup);
    if (rc != 0) {
        return fail_install(tx, claim.id, "native hook failed", out);
    }

    runtime_set(claim.id, entry, backup);
    set_state(claim.id, want ? contracts::ResourceState::kLive
                             : contracts::ResourceState::kDisabled);
    tx.entries[index].installed = true;
    ++tx.step;
    out->resource_id = claim.id;
    out->ok = 0;
    out->backup = backup;
    out->finished = tx.step >= tx.total;
    if (out->finished) {
        tx.state = TxState::kSucceeded;
        finalize_terminal(tx);
    }
    return !out->finished;
}

bool restore_step(Tx& tx, TxStep* out) {
    if (tx.step >= tx.total) {
        tx.state = TxState::kSucceeded;
        out->finished = true;
        finalize_terminal(tx);
        return false;
    }
    const uint32_t index = tx.total - 1 - tx.step;  // 逆序还原
    TxEntry& entry = tx.entries[index];
    const contracts::ResourceClaim& claim = *entry.claim;
    tx.budget = budget_of(claim);

    const contracts::ResourceState state = state_of(claim.id);
    if (state != contracts::ResourceState::kLive && state != contracts::ResourceState::kDisabled) {
        // 无可还原：跳过并推进。
        ++tx.step;
        out->resource_id = claim.id;
        out->ok = 0;
        out->finished = tx.step >= tx.total;
        if (out->finished) {
            tx.state = TxState::kSucceeded;
            finalize_terminal(tx);
        }
        return !out->finished;
    }

    if (isolated_get(claim.id)) {
        return fail_restore(tx, claim.id, "isolated", out);
    }
    // 机制分派：非入口机制无 wrapper、无在途等待，直接释放。
    if (claim.mechanism != contracts::ResourceMechanism::kEntryHook) {
        return restore_other_step(tx, claim, out);
    }

    if (in_flight_count(claim.id) > 0) {
        // 在途未归零：不推进步骤，只消耗帧；超该资源预算则停止等待并隔离。
        ++entry.waited_frames;
        if (entry.waited_frames > budget_of(claim)) {
            set_state(claim.id, contracts::ResourceState::kTombstone);
            isolated_set(claim.id, true);
            return fail_restore(tx, claim.id, "in_flight_timeout", out);
        }
        out->resource_id = nullptr;  // 等待帧
        out->ok = 0;
        out->finished = false;
        return true;
    }

    if (!claim.resumable) {
        set_state(claim.id, contracts::ResourceState::kTombstone);
        isolated_set(claim.id, true);
        return fail_restore(tx, claim.id, "not resumable", out);
    }

    ResourceRuntime runtime{};
    const uintptr_t entry_address = runtime_of(claim.id, &runtime) ? runtime.resolved_entry : 0;
    const ReleaseOutcome outcome = release_claim_entry_hook(claim, entry_address, g_deps.unhook_fn);
    if (outcome.ok != 0) {
        set_state(claim.id, contracts::ResourceState::kTombstone);
        isolated_set(claim.id, true);
        return fail_restore(tx, claim.id, outcome.error, out);
    }

    ++tx.step;
    out->resource_id = claim.id;
    out->ok = 0;
    out->finished = tx.step >= tx.total;
    if (out->finished) {
        tx.state = TxState::kSucceeded;
        finalize_terminal(tx);
    }
    return !out->finished;
}

}  // namespace

void tx_set_deps(const TxDeps& deps) {
    g_deps = deps;
    g_deps_set = true;
}

uint64_t tx_begin(const char* plugin_id, TxKind kind) {
    if (!g_deps_set || is_empty(plugin_id)) {
        return 0;
    }
    // 必填原语缺失等同于 deps 未装配，拒绝而非空指针调用。
    if (g_deps.resolve_entry == nullptr || g_deps.hook_fn == nullptr || g_deps.unhook_fn == nullptr) {
        return 0;
    }
    // 同插件未终态事务唯一：唯一执行器由宿主保证在主线程。
    if (find_active_by_plugin(plugin_id) != nullptr) {
        return 0;
    }

    // 先校验资源与隔离标记，再占用槽位。
    uint32_t total = 0;
    const uint32_t claims = claim_count();
    for (uint32_t i = 0; i < claims; ++i) {
        const contracts::ResourceClaim* claim = claim_at(i);
        if (claim == nullptr || claim->owner == nullptr) {
            continue;
        }
        if (std::strcmp(claim->owner, plugin_id) != 0) {
            continue;
        }
        // 隔离后禁止重试，直到进程重启。
        if (isolated_get(claim->id)) {
            return 0;
        }
        ++total;
    }
    if (total == 0) {
        return 0;
    }

    Tx* slot = nullptr;
    for (Tx& candidate : g_active) {
        if (!candidate.in_use) {
            slot = &candidate;
            break;
        }
    }
    if (slot == nullptr) {
        return 0;
    }

    Tx& tx = *slot;
    tx.id = g_next_id++;
    tx.plugin_id = plugin_id;
    tx.kind = kind;
    tx.state = TxState::kRunning;
    tx.step = 0;
    tx.total = total;
    tx.frames = 0;
    tx.budget = kDefaultBudget;
    tx.failed_resource_id = nullptr;
    tx.error = nullptr;

    uint32_t index = 0;
    for (uint32_t i = 0; i < claims; ++i) {
        const contracts::ResourceClaim* claim = claim_at(i);
        if (claim == nullptr || claim->owner == nullptr) {
            continue;
        }
        if (std::strcmp(claim->owner, plugin_id) != 0) {
            continue;
        }
        tx.entries[index].claim = claim;
        tx.entries[index].installed = false;
        tx.entries[index].waited_frames = 0;
        ++index;
    }
    tx.budget = budget_of(*tx.entries[0].claim);
    tx.in_use = true;
    return tx.id;
}

bool tx_step(uint64_t tx_id, TxStep* out) {
    if (out == nullptr) {
        return false;
    }
    Tx* tx = find_active(tx_id);
    if (tx == nullptr) {
        return false;
    }
    out->resource_id = nullptr;
    out->ok = 0;
    out->backup = nullptr;
    out->finished = false;

    // 每次调用消耗 1 帧，等待帧同样计入。
    ++tx->frames;

    if (tx->kind == TxKind::kInstall) {
        return install_step(*tx, out);
    }
    return restore_step(*tx, out);
}

bool tx_query(uint64_t tx_id, TxStatus* out) {
    if (out == nullptr || tx_id == 0) {
        return false;
    }
    for (const Tx& tx : g_active) {
        if (tx.in_use && tx.id == tx_id) {
            fill_status(tx, out);
            return true;
        }
    }
    for (const TerminalRecord& record : g_terminal) {
        if (record.used && record.id == tx_id) {
            *out = record.status;
            return true;
        }
    }
    return false;
}

void tx_reset_for_test() {
    for (Tx& tx : g_active) {
        tx = Tx{};
    }
    for (TerminalRecord& record : g_terminal) {
        record = TerminalRecord{};
    }
    g_terminal_next = 0;
    g_deps = TxDeps{};
    g_deps_set = false;
    g_next_id = 1;
}

}  // namespace qol::hooks
