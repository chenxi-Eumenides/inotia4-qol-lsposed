#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <set>
#include <vector>

#include <sys/mman.h>

#include "core/native/near_code_alloc.h"
#include "core/native/near_thunk_rules.h"

namespace {

int g_pass = 0;
int g_fail = 0;

#define CHECK(condition)                                                         \
    do {                                                                         \
        if (condition) {                                                         \
            ++g_pass;                                                            \
        } else {                                                                 \
            ++g_fail;                                                            \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition);    \
        }                                                                        \
    } while (false)

struct PortState {
    size_t mmap_calls = 0;
    size_t munmap_calls = 0;
    size_t mismatch_count = 0;
    bool always_mismatch = false;
    uint64_t now_ms = 0;
    std::vector<uintptr_t> mapped_pages;
};

bool has_page(const PortState& state, uintptr_t page) {
    return std::find(state.mapped_pages.begin(), state.mapped_pages.end(), page) !=
           state.mapped_pages.end();
}

void* stub_mmap(void* context, void* address, size_t, int, int flags, int, off_t) {
    auto* state = static_cast<PortState*>(context);
    ++state->mmap_calls;
    const uintptr_t candidate = reinterpret_cast<uintptr_t>(address);
    if (state->mismatch_count != 0) {
        --state->mismatch_count;
        return reinterpret_cast<void*>(candidate + near_thunk_rules::kPageSize);
    }
#ifdef MAP_FIXED_NOREPLACE
    if ((flags & MAP_FIXED_NOREPLACE) != 0) {
        if (has_page(*state, candidate)) return MAP_FAILED;
        state->mapped_pages.push_back(candidate);
        return address;
    }
#endif
    if (state->always_mismatch) {
        return reinterpret_cast<void*>(candidate + near_thunk_rules::kPageSize);
    }
    if (has_page(*state, candidate)) {
        return reinterpret_cast<void*>(candidate + near_thunk_rules::kPageSize);
    }
    state->mapped_pages.push_back(candidate);
    return address;
}

int stub_munmap(void* context, void* address, size_t) {
    auto* state = static_cast<PortState*>(context);
    ++state->munmap_calls;
    const uintptr_t page = reinterpret_cast<uintptr_t>(address);
    const auto it = std::find(state->mapped_pages.begin(), state->mapped_pages.end(), page);
    if (it != state->mapped_pages.end()) state->mapped_pages.erase(it);
    return 0;
}

uint64_t stub_now(void* context) {
    return static_cast<PortState*>(context)->now_ms;
}

NearAllocPort test_port(PortState* state, bool use_fixed_noreplace = false) {
    return {state, stub_mmap, stub_munmap, use_fixed_noreplace};
}

void test_candidate_order_and_bounds() {
    const uintptr_t target = 0x40000000;
    const std::vector<uintptr_t> candidates = near_thunk_rules::enumerate_candidates(target);
    CHECK(!candidates.empty());
    CHECK(candidates[0] == target + near_thunk_rules::kCoarseStep);
    CHECK(candidates[1] == target - near_thunk_rules::kCoarseStep);
    CHECK(candidates[2] == target + 2 * near_thunk_rules::kCoarseStep);
    for (const uintptr_t candidate : candidates) {
        CHECK(near_thunk_rules::page_aligned(candidate));
        CHECK(near_thunk_rules::bl_reachable(target, candidate));
    }

    std::set<uintptr_t> unique(candidates.begin(), candidates.end());
    CHECK(unique.size() == candidates.size());
    size_t coarse_count = 0;
    size_t fine_count = 0;
    for (const uintptr_t candidate : candidates) {
        const uintptr_t distance = candidate >= target ? candidate - target : target - candidate;
        if (distance % near_thunk_rules::kCoarseStep == 0) {
            ++coarse_count;
        } else {
            ++fine_count;
        }
    }
    const size_t coarse_distances = near_thunk_rules::kBlRange / near_thunk_rules::kCoarseStep - 1;
    const size_t fine_distances = near_thunk_rules::kBlRange / near_thunk_rules::kFineStep - 1;
    CHECK(coarse_count == 2 * coarse_distances);
    CHECK(fine_count == 2 * (fine_distances - coarse_distances));
}

void test_range_alignment_and_overflow() {
    const uintptr_t target = 0x50000000;
    CHECK(near_thunk_rules::bl_reachable(target, target + near_thunk_rules::kBlRange -
                                         near_thunk_rules::kPageSize));
    CHECK(!near_thunk_rules::bl_reachable(target, target + near_thunk_rules::kBlRange));
    CHECK(near_thunk_rules::bl_reachable(target, target - near_thunk_rules::kBlRange +
                                         near_thunk_rules::kPageSize));
    CHECK(!near_thunk_rules::bl_reachable(target, target - near_thunk_rules::kBlRange));
    CHECK(!near_thunk_rules::bl_reachable(target, target + near_thunk_rules::kFineStep + 1));
    CHECK(!near_thunk_rules::bl_reachable(target + 1, target + near_thunk_rules::kFineStep));
    CHECK(!near_thunk_rules::checked_add(std::numeric_limits<uintptr_t>::max() - 1, 2,
                                         nullptr));
    uintptr_t out = 0;
    CHECK(!near_thunk_rules::checked_add(std::numeric_limits<uintptr_t>::max() - 1, 2, &out));
    CHECK(!near_thunk_rules::checked_sub(0, 1, &out));
    const uintptr_t high_target = std::numeric_limits<uintptr_t>::max() -
                                  near_thunk_rules::kPageSize + 1;
    const uintptr_t high_base = high_target & ~(near_thunk_rules::kPageSize - 1);
    const std::vector<uintptr_t> high_candidates =
        near_thunk_rules::enumerate_candidates(high_target);
    CHECK(!high_candidates.empty());
    for (const uintptr_t candidate : high_candidates) {
        CHECK(near_thunk_rules::page_aligned(candidate));
        CHECK(near_thunk_rules::bl_reachable(high_target, candidate));
        const uintptr_t distance = candidate >= high_target ? candidate - high_target
                                                            : high_target - candidate;
        CHECK(distance < near_thunk_rules::kBlRange);
        uintptr_t reconstructed = 0;
        if (candidate >= high_base) {
            CHECK(near_thunk_rules::checked_add(high_base, candidate - high_base,
                                                &reconstructed));
        } else {
            CHECK(near_thunk_rules::checked_sub(high_base, high_base - candidate,
                                                &reconstructed));
        }
        CHECK(reconstructed == candidate);
    }
}

void test_hint_rejects_unexpected_and_continues() {
    PortState state;
    state.mismatch_count = 1;
    NearCodeAllocator allocator(test_port(&state));
    const uintptr_t target = 0x40000000;
    void* page = allocator.page_alloc(target);
    CHECK(reinterpret_cast<uintptr_t>(page) == target - near_thunk_rules::kCoarseStep);
    CHECK(state.munmap_calls == 1);
}

void test_hint_exact_return_is_accepted() {
    PortState state;
    NearCodeAllocator allocator(test_port(&state));
    const uintptr_t target = 0x40000000;
    void* page = allocator.page_alloc(target);
    CHECK(reinterpret_cast<uintptr_t>(page) == target + near_thunk_rules::kCoarseStep);
    CHECK(state.munmap_calls == 0);
}

void test_fixed_optimization_also_validates_return() {
    PortState state;
    state.mismatch_count = 1;
    NearCodeAllocator allocator(test_port(&state, true));
    const uintptr_t target = 0x40000000;
    void* page = allocator.page_alloc(target);
    CHECK(reinterpret_cast<uintptr_t>(page) == target - near_thunk_rules::kCoarseStep);
    CHECK(state.munmap_calls == 1);
}

void test_final_mismatch_releases_every_unexpected_region() {
    PortState state;
    state.always_mismatch = true;
    NearCodeAllocator allocator(test_port(&state));
    CHECK(allocator.page_alloc(0x40000000) == nullptr);
    CHECK(state.mmap_calls > 0);
    CHECK(state.munmap_calls == state.mmap_calls);
}

void test_128_slots_and_cross_call_reuse() {
    PortState state;
    NearCodeAllocator allocator(test_port(&state));
    const uintptr_t target = 0x40000000;
    std::vector<void*> slots;
    for (size_t index = 0; index < 128; ++index) {
        slots.push_back(allocator.slot_alloc(target, near_thunk_rules::kThunkSlotSize));
        CHECK(slots.back() != nullptr);
        const uintptr_t expected = reinterpret_cast<uintptr_t>(slots.front()) +
                                   index * near_thunk_rules::kThunkSlotSize;
        CHECK(reinterpret_cast<uintptr_t>(slots.back()) == expected);
    }
    void* next_page = allocator.slot_alloc(target, near_thunk_rules::kThunkSlotSize);
    CHECK(next_page != nullptr);
    CHECK((reinterpret_cast<uintptr_t>(next_page) & (near_thunk_rules::kPageSize - 1)) == 0);
    CHECK(next_page != slots.front());

    void* distant_slot = allocator.slot_alloc(target + near_thunk_rules::kBlRange + 0x100000,
                                              near_thunk_rules::kThunkSlotSize);
    CHECK(distant_slot != nullptr);
    CHECK(distant_slot != next_page);
}

void test_partial_page_reuse_and_remote_rejection() {
    PortState state;
    NearCodeAllocator allocator(test_port(&state));
    const uintptr_t target = 0x40000000;
    void* first = allocator.slot_alloc(target, near_thunk_rules::kThunkSlotSize);
    CHECK(first != nullptr);
    const size_t calls_after_first = state.mmap_calls;

    void* near_slot = allocator.slot_alloc(target, near_thunk_rules::kThunkSlotSize);
    CHECK(reinterpret_cast<uintptr_t>(near_slot) ==
          reinterpret_cast<uintptr_t>(first) + near_thunk_rules::kThunkSlotSize);
    CHECK(state.mmap_calls == calls_after_first);

    const uintptr_t remote_target = target + near_thunk_rules::kBlRange + 0x100000;
    void* remote_slot = allocator.slot_alloc(remote_target, near_thunk_rules::kThunkSlotSize);
    CHECK(remote_slot != nullptr);
    CHECK(remote_slot != first);
    CHECK(state.mmap_calls == calls_after_first + 1);
}

void test_failure_backoff() {
    PortState state;
    state.always_mismatch = true;
    NearAllocPort port = test_port(&state);
    port.now_fn = stub_now;
    NearCodeAllocator allocator(port);
    const uintptr_t target = 0x40000000;

    CHECK(allocator.page_alloc(target) == nullptr);
    const size_t first_scan_mmaps = state.mmap_calls;
    CHECK(first_scan_mmaps > 0);
    CHECK(allocator.page_alloc(target) == nullptr);
    CHECK(state.mmap_calls == first_scan_mmaps);

    state.now_ms = kNearCodeFailureBackoffMs;
    CHECK(allocator.page_alloc(target) == nullptr);
    const size_t second_scan_mmaps = state.mmap_calls;
    CHECK(second_scan_mmaps > first_scan_mmaps);
    CHECK(allocator.page_alloc(target) == nullptr);
    CHECK(state.mmap_calls == second_scan_mmaps);
}

}  // namespace

int main() {
    test_candidate_order_and_bounds();
    test_range_alignment_and_overflow();
    test_hint_rejects_unexpected_and_continues();
    test_hint_exact_return_is_accepted();
    test_fixed_optimization_also_validates_return();
    test_final_mismatch_releases_every_unexpected_region();
    test_128_slots_and_cross_call_reuse();
    test_partial_page_reuse_and_remote_rejection();
    test_failure_backoff();
    std::printf("near_thunk_rules: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
