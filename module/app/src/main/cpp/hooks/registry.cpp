#include "hooks/registry.h"

// 资源登记表实现（见 hooks/registry.h 的接口契约）。
// 静态数组 + 追加计数：登记只在装配期发生，无删除路径，故不需要空闲链表。

#include <atomic>
#include <cstdint>
#include <cstring>

namespace qol::hooks {
namespace {

constexpr uint32_t kMaxClaims = 128;

struct Entry {
    contracts::ResourceClaim claim;
    contracts::ResourceState state;
    // 运行期字段（P3 资源事务引擎）：安装成功写入，物理释放清除。
    uintptr_t resolved_entry;
    void* backup;
    std::atomic<uint32_t> in_flight;  // 跨帧等待在途归零，读写均需原子
    bool isolated;                    // 隔离后禁止重试，直到进程重启
    bool retry_pending;               // 常驻 fail-soft 资源安装失败待重试；成功安装/归还清除
};

Entry g_entries[kMaxClaims];
uint32_t g_count = 0;

// 按 id 查条目：登记、状态迁移与查询共用同一比较规则（strcmp）。
Entry* find_entry(const char* resource_id) {
    if (resource_id == nullptr) {
        return nullptr;
    }
    for (uint32_t i = 0; i < g_count; ++i) {
        if (std::strcmp(g_entries[i].claim.id, resource_id) == 0) {
            return &g_entries[i];
        }
    }
    return nullptr;
}

}  // namespace

bool register_claim(const contracts::ResourceClaim& claim) {
    if (claim.id == nullptr || claim.symbol == nullptr || claim.owner == nullptr) {
        return false;
    }
    Entry* existing = find_entry(claim.id);
    if (existing != nullptr) {
        // 一资源一安装者：不同 owner 拒绝；同 owner 幂等，保留首次登记。
        return std::strcmp(existing->claim.owner, claim.owner) == 0;
    }
    if (g_count >= kMaxClaims) {
        return false;
    }
    Entry& entry = g_entries[g_count];
    entry.claim = claim;
    // 已声明但尚未安装：占用状态为 free，安装引擎成功后再置 disabled/live。
    entry.state = contracts::ResourceState::kFree;
    // 槽位可能被上一轮测试复用，运行期字段显式清零。
    entry.resolved_entry = 0;
    entry.backup = nullptr;
    entry.in_flight.store(0);
    entry.isolated = false;
    entry.retry_pending = false;
    ++g_count;
    return true;
}

const contracts::ResourceClaim* find_claim(const char* resource_id) {
    const Entry* entry = find_entry(resource_id);
    return entry == nullptr ? nullptr : &entry->claim;
}

contracts::ResourceState state_of(const char* resource_id) {
    const Entry* entry = find_entry(resource_id);
    return entry == nullptr ? contracts::ResourceState::kFree : entry->state;
}

bool set_state(const char* resource_id, contracts::ResourceState state) {
    Entry* entry = find_entry(resource_id);
    if (entry == nullptr) {
        return false;
    }
    entry->state = state;
    return true;
}

uint32_t claim_count() {
    return g_count;
}

const contracts::ResourceClaim* claim_at(uint32_t index) {
    if (index >= g_count) {
        return nullptr;
    }
    return &g_entries[index].claim;
}

uint32_t count_by_state(contracts::ResourceState state) {
    uint32_t count = 0;
    for (uint32_t i = 0; i < g_count; ++i) {
        if (g_entries[i].state == state) {
            ++count;
        }
    }
    return count;
}

bool runtime_set(const char* resource_id, uintptr_t resolved_entry, void* backup) {
    Entry* entry = find_entry(resource_id);
    if (entry == nullptr) {
        return false;
    }
    entry->resolved_entry = resolved_entry;
    entry->backup = backup;
    // 安装成功即清除定向重试标记。
    entry->retry_pending = false;
    return true;
}

bool runtime_clear(const char* resource_id) {
    Entry* entry = find_entry(resource_id);
    if (entry == nullptr) {
        return false;
    }
    entry->resolved_entry = 0;
    entry->backup = nullptr;
    // 物理释放以在途归零为前提，清零避免残留计数阻塞后续复用。
    entry->in_flight.store(0);
    // 归还成功即清除定向重试标记。
    entry->retry_pending = false;
    return true;
}

bool runtime_of(const char* resource_id, ResourceRuntime* out) {
    if (out == nullptr) {
        return false;
    }
    const Entry* entry = find_entry(resource_id);
    // resolved_entry==0 表示运行期未写（或已清除），与「未登记」同样返回 false。
    if (entry == nullptr || entry->resolved_entry == 0) {
        return false;
    }
    out->resolved_entry = entry->resolved_entry;
    out->backup = entry->backup;
    out->in_flight = entry->in_flight.load();
    out->isolated = entry->isolated;
    return true;
}

bool entry_target_conflicts(uintptr_t resolved_entry, const char* resource_id) {
    if (resolved_entry == 0 || resource_id == nullptr) return false;
    const Entry* requested = find_entry(resource_id);
    if (requested == nullptr ||
        requested->claim.mechanism != contracts::ResourceMechanism::kEntryHook) return false;
    for (uint32_t i = 0; i < g_count; ++i) {
        const Entry& entry = g_entries[i];
        if (entry.claim.mechanism != contracts::ResourceMechanism::kEntryHook ||
            std::strcmp(entry.claim.id, resource_id) == 0) continue;
        const bool occupied = entry.state == contracts::ResourceState::kLive ||
                              entry.state == contracts::ResourceState::kDisabled ||
                              entry.state == contracts::ResourceState::kTombstone;
        if (occupied && entry.resolved_entry == resolved_entry) return true;
    }
    return false;
}

bool in_flight_enter(const char* resource_id) {
    Entry* entry = find_entry(resource_id);
    if (entry == nullptr) {
        return false;
    }
    entry->in_flight.fetch_add(1);
    return true;
}

bool in_flight_leave(const char* resource_id) {
    Entry* entry = find_entry(resource_id);
    if (entry == nullptr) {
        return false;
    }
    // 下界 0：归零后重复 leave 不得回绕成极大值。
    uint32_t current = entry->in_flight.load();
    while (current > 0) {
        if (entry->in_flight.compare_exchange_weak(current, current - 1)) {
            break;
        }
    }
    return true;
}

uint32_t in_flight_count(const char* resource_id) {
    const Entry* entry = find_entry(resource_id);
    return entry == nullptr ? 0 : entry->in_flight.load();
}

bool isolated_set(const char* resource_id, bool isolated) {
    Entry* entry = find_entry(resource_id);
    if (entry == nullptr) {
        return false;
    }
    entry->isolated = isolated;
    return true;
}

bool isolated_get(const char* resource_id) {
    const Entry* entry = find_entry(resource_id);
    return entry != nullptr && entry->isolated;
}

bool retry_pending_set(const char* resource_id, bool pending) {
    Entry* entry = find_entry(resource_id);
    if (entry == nullptr) {
        return false;
    }
    entry->retry_pending = pending;
    return true;
}

bool retry_pending_get(const char* resource_id) {
    const Entry* entry = find_entry(resource_id);
    return entry != nullptr && entry->retry_pending;
}

void registry_reset_for_test() {
    // 全表清零：register_claim 复用槽位时不再单独依赖此清理，但重入测试需彻底复位。
    for (uint32_t i = 0; i < kMaxClaims; ++i) {
        g_entries[i].resolved_entry = 0;
        g_entries[i].backup = nullptr;
        g_entries[i].in_flight.store(0);
        g_entries[i].isolated = false;
        g_entries[i].retry_pending = false;
    }
    g_count = 0;
}

}  // namespace qol::hooks
