#include "hooks/registry.h"

// 资源登记表实现（见 hooks/registry.h 的接口契约）。
// 静态数组 + 追加计数：登记只在装配期发生，无删除路径，故不需要空闲链表。

#include <cstdint>
#include <cstring>

namespace qol::hooks {
namespace {

constexpr uint32_t kMaxClaims = 64;

struct Entry {
    contracts::ResourceClaim claim;
    contracts::ResourceState state;
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
    g_entries[g_count].claim = claim;
    // 已声明但尚未安装：占用状态为 free，安装引擎成功后再置 disabled/live。
    g_entries[g_count].state = contracts::ResourceState::kFree;
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

void registry_reset_for_test() {
    g_count = 0;
}

}  // namespace qol::hooks
