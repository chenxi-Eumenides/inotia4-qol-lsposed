#include "patch/claims.h"

#include <cstring>

namespace qol::patch {
namespace {

constexpr uint32_t kMaxClaims = 64;

struct PointClaim {
    uintptr_t addr;
    const char* claim_id;
};

PointClaim g_claims[kMaxClaims];
uint32_t g_count = 0;

// 按地址查下标；未占用返回 -1。
int find_index(uintptr_t addr) {
    for (uint32_t i = 0; i < g_count; ++i) {
        if (g_claims[i].addr == addr) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

}  // namespace

bool claim_point(uintptr_t addr, const char* claim_id) {
    if (claim_id == nullptr || claim_id[0] == '\0') {
        return false;
    }
    const int index = find_index(addr);
    if (index >= 0) {
        // 一地址一所有者：不同 claim 拒绝；同 claim 幂等。
        return std::strcmp(g_claims[index].claim_id, claim_id) == 0;
    }
    if (g_count >= kMaxClaims) {
        return false;
    }
    g_claims[g_count].addr = addr;
    g_claims[g_count].claim_id = claim_id;
    ++g_count;
    return true;
}

bool release_point(uintptr_t addr) {
    const int index = find_index(addr);
    if (index < 0) {
        return false;
    }
    // 用末位覆盖，保持紧凑；释放顺序与声明顺序无关。
    g_claims[index] = g_claims[g_count - 1];
    --g_count;
    return true;
}

const char* point_owner(uintptr_t addr) {
    const int index = find_index(addr);
    return index < 0 ? nullptr : g_claims[index].claim_id;
}

uint32_t claimed_count() {
    return g_count;
}

void claims_reset_for_test() {
    g_count = 0;
}

}  // namespace qol::patch
