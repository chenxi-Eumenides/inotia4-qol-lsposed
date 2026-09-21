#ifndef INOTIA4_QOL_NEAR_THUNK_RULES_H
#define INOTIA4_QOL_NEAR_THUNK_RULES_H

#include <cstdint>
#include <limits>
#include <vector>

namespace near_thunk_rules {

constexpr uintptr_t kPageSize = 4096;
constexpr uintptr_t kBlRange = 0x08000000;
constexpr uintptr_t kCoarseStep = 0x10000;
constexpr uintptr_t kFineStep = 0x1000;
constexpr size_t kThunkSlotSize = 32;

inline bool page_aligned(uintptr_t address) {
    return (address & (kPageSize - 1)) == 0;
}

inline bool branch_reachable(uintptr_t target, uintptr_t candidate) {
    if (target == 0 || candidate == 0) return false;
    const uintptr_t distance = candidate >= target ? candidate - target : target - candidate;
    return (distance & 0x3u) == 0 && distance < kBlRange;
}

inline bool bl_reachable(uintptr_t target, uintptr_t candidate) {
    return page_aligned(candidate) && branch_reachable(target, candidate);
}

inline bool checked_add(uintptr_t value, uintptr_t amount, uintptr_t* out) {
    if (out == nullptr || amount > std::numeric_limits<uintptr_t>::max() - value) return false;
    *out = value + amount;
    return true;
}

inline bool checked_sub(uintptr_t value, uintptr_t amount, uintptr_t* out) {
    if (out == nullptr || amount > value) return false;
    *out = value - amount;
    return true;
}

inline std::vector<uintptr_t> enumerate_candidates(uintptr_t target) {
    std::vector<uintptr_t> result;
    if (target == 0) return result;

    const uintptr_t base = target & ~(kPageSize - 1);
    for (const uintptr_t step : {kCoarseStep, kFineStep}) {
        for (uintptr_t distance = step; distance < kBlRange;) {
            // Every coarse offset is also present in the fine sweep; attempt it only once.
            if (step != kFineStep || distance % kCoarseStep != 0) {
                uintptr_t candidate = 0;
                if (checked_add(base, distance, &candidate) && bl_reachable(target, candidate)) {
                    result.push_back(candidate);
                }
                if (checked_sub(base, distance, &candidate) && bl_reachable(target, candidate)) {
                    result.push_back(candidate);
                }
            }
            if (distance > kBlRange - step) break;
            distance += step;
        }
    }
    return result;
}

}  // namespace near_thunk_rules

#endif
