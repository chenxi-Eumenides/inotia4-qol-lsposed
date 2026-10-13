// call_patch.cpp —— 通用 BL 调用点 patch（自动出售阶段 A 基础设施）。
//
// thunk/mprotect 逻辑源自扩展背包原 allocate_draw_thunk 的近址页 carving；D2a-3b-2 后
// 扩展背包改用本机制，本文件为唯一实现。

#include "patch/call_patch.h"

#include "infra/log/qol_log.h"

#include <array>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <mutex>
#include <sys/mman.h>
#include <sys/utsname.h>

namespace {

constexpr size_t kPageSize = 4096;
constexpr size_t kThunkSlot = 32;         // 每槽 32B 对齐（thunk 码 20B + 字面量 8B）
constexpr size_t kSlotsPerPage = kPageSize / kThunkSlot;
constexpr uintptr_t kBlNegativeReach = uintptr_t{1} << 27;
constexpr uintptr_t kBlPositiveReach = (uintptr_t{1} << 27) - 4;
// Linux MAP_FIXED_NOREPLACE ABI value, including on build headers predating its declaration.
constexpr int kMapFixedNoReplace = 0x100000;

std::mutex g_call_patch_mtx;

enum class MappingMode : uint8_t { kUnknown, kNoReplace, kHint };
MappingMode g_mapping_mode = MappingMode::kUnknown;

using MmapFunction = decltype(&::mmap);
using MunmapFunction = decltype(&::munmap);
MmapFunction g_mmap_function = &::mmap;
MunmapFunction g_munmap_function = &::munmap;

// thunk 页登记：每页 4096B 承载 32B thunk，共享复用；页必落在调用点 ±128MB 内。
std::array<void*, 8> g_thunk_pages{};
size_t g_thunk_page_count = 0;
std::array<size_t, 8> g_thunk_page_used{};
std::array<void*, 8 * kSlotsPerPage> g_free_thunk_slots{};
size_t g_free_thunk_slot_count = 0;
size_t g_thunk_slots_allocated = 0;

// 已安装调用点登记（M-1 幂等）：同一 call_addr + 同一 wrapper 重复 install 直接
// 返回 true，避免重复分配 thunk。容量上限仅用于避免无界增长；溢出时退化为原行为。
struct PatchedCall {
    uintptr_t call_addr;
    void* wrapper;
    uint32_t saved_word;
    uint32_t replacement;
    uintptr_t target;  // 实际写入的分支目标（BL：wrapper/thunk；B：thunk）
    void* thunk_slot;
};
constexpr size_t kMaxPatchedCalls = 128;
std::array<PatchedCall, kMaxPatchedCalls> g_patched_calls{};
size_t g_patched_call_count = 0;

struct AllocationFailure {
    uintptr_t requested = 0;
    uintptr_t actual = 0;
    const char* stage = "scan_exhausted";
    int saved_errno = 0;
};

// out_target 可空；命中时回填上次写入的分支目标（B 幂等复用既有 thunk 用）。
bool find_patched_locked(uintptr_t call_addr, void* wrapper, uintptr_t* out_target = nullptr) {
    for (size_t i = 0; i < g_patched_call_count; ++i) {
        if (g_patched_calls[i].call_addr == call_addr && g_patched_calls[i].wrapper == wrapper) {
            if (out_target != nullptr) *out_target = g_patched_calls[i].target;
            return true;
        }
    }
    return false;
}

bool remember_patched_locked(uintptr_t call_addr, void* wrapper, uint32_t replacement,
                             uint32_t saved_word, uintptr_t target, void* thunk_slot) {
    if (find_patched_locked(call_addr, wrapper)) return true;
    if (g_patched_call_count >= g_patched_calls.size()) {
        QOL_LOG_ERROR(QolDomain::kPlatform,
                      "patched call table capacity exhausted limit=%zu call=%p wrapper=%p",
                      g_patched_calls.size(), reinterpret_cast<void*>(call_addr), wrapper);
        return false;
    }
    g_patched_calls[g_patched_call_count++] =
        {call_addr, wrapper, saved_word, replacement, target, thunk_slot};
    return true;
}

bool saved_word_locked(uintptr_t call_addr, uint32_t* out_word) {
    for (size_t i = 0; i < g_patched_call_count; ++i) {
        if (g_patched_calls[i].call_addr == call_addr) {
            if (out_word != nullptr) *out_word = g_patched_calls[i].saved_word;
            return true;
        }
    }
    return false;
}

void release_thunk_slot_locked(void* slot) {
    if (slot == nullptr) return;
    if (g_free_thunk_slot_count >= g_free_thunk_slots.size()) {
        QOL_LOG_ERROR(QolDomain::kPlatform, "thunk free list capacity exhausted slot=%p", slot);
        return;
    }
    g_free_thunk_slots[g_free_thunk_slot_count++] = slot;
}

void forget_patched_locked(uintptr_t call_addr) {
    for (size_t i = 0; i < g_patched_call_count;) {
        if (g_patched_calls[i].call_addr == call_addr) {
            release_thunk_slot_locked(g_patched_calls[i].thunk_slot);
            g_patched_calls[i] = g_patched_calls[g_patched_call_count - 1];
            --g_patched_call_count;
        } else {
            ++i;
        }
    }
}

void emit_call_thunk(void* slot, uintptr_t wrapper) {
    uint32_t code[] = {
        0xa9bf7bf0,  // stp x16, x30, [sp, #-16]!
        0x58000090,  // ldr x16, #16 (literal at thunk+20)
        0xd63f0200,  // blr x16
        0xa8c17bf0,  // ldp x16, x30, [sp], #16
        0xd65f03c0,  // ret
    };
    std::memcpy(slot, code, sizeof(code));
    *reinterpret_cast<uintptr_t*>(reinterpret_cast<uint8_t*>(slot) + 20) = wrapper;
    __builtin___clear_cache(reinterpret_cast<char*>(slot),
                            reinterpret_cast<char*>(reinterpret_cast<uint8_t*>(slot) + kThunkSlot));
}

// B 尾调用 thunk：`ldr x16, #8; br x16` + thunk+8 的 handler 字面量。B 不改写 x30，
// `br` 进入 handler 后其 `ret` 直接返回原调用者，等价原尾调用。
void emit_branch_thunk(void* slot, uintptr_t handler) {
    uint32_t code[] = {
        0x58000050,  // ldr x16, #8 (literal at thunk+8)
        0xd61f0200,  // br x16
    };
    std::memcpy(slot, code, sizeof(code));
    *reinterpret_cast<uintptr_t*>(reinterpret_cast<uint8_t*>(slot) + 8) = handler;
    __builtin___clear_cache(reinterpret_cast<char*>(slot),
                            reinterpret_cast<char*>(reinterpret_cast<uint8_t*>(slot) + kThunkSlot));
}

bool branch_delta(uintptr_t from, uintptr_t to, int64_t* out_delta) {
    if (from == 0 || to == 0 || out_delta == nullptr || ((from | to) & 0x3u) != 0) {
        return false;
    }
    if (to >= from) {
        const uintptr_t distance = to - from;
        if (distance > kBlPositiveReach) return false;
        *out_delta = static_cast<int64_t>(distance);
        return true;
    }
    const uintptr_t distance = from - to;
    if (distance > kBlNegativeReach) return false;
    *out_delta = -static_cast<int64_t>(distance);
    return true;
}

bool encode_branch(uintptr_t from, uintptr_t to, uint32_t* out) {
    int64_t delta = 0;
    if (out == nullptr || !branch_delta(from, to, &delta)) return false;
    *out = 0x14000000u | (static_cast<uint32_t>(delta / 4) & 0x03ffffffu);
    return true;
}

bool candidate_address(uintptr_t base, uint64_t distance, bool positive,
                       uintptr_t* out_candidate) {
    if (out_candidate == nullptr || distance > std::numeric_limits<uintptr_t>::max()) {
        return false;
    }
    const uintptr_t offset = static_cast<uintptr_t>(distance);
    if (positive) {
        if (offset > std::numeric_limits<uintptr_t>::max() - base) return false;
        *out_candidate = base + offset;
        return *out_candidate != 0;
    }
    if (offset >= base) return false;
    *out_candidate = base - offset;
    return *out_candidate != 0;
}

void emit_branch_return_thunk(void* slot, uintptr_t handler, uint32_t return_branch,
                              uint32_t tail_word) {
    const uint32_t code[] = {
        0x58000090u,  // ldr x16, #16
        0xd63f0200u,  // blr x16
        tail_word,    // fixed epilogue value, normally mov x0,#1
        return_branch,
    };
    std::memcpy(slot, code, sizeof(code));
    *reinterpret_cast<uintptr_t*>(reinterpret_cast<uint8_t*>(slot) + 16) = handler;
    __builtin___clear_cache(reinterpret_cast<char*>(slot),
                            reinterpret_cast<char*>(reinterpret_cast<uint8_t*>(slot) + 24));
}

void* acquire_existing_thunk_slot_locked(uintptr_t call_addr) {
    uint32_t ignored_branch = 0;
    // 先复用同一近址范围内的空闲槽；每个槽均按当前站点重新检查精确 BL 可达性。
    for (size_t i = g_free_thunk_slot_count; i > 0; --i) {
        void* slot = g_free_thunk_slots[i - 1];
        if (encode_branch(call_addr, reinterpret_cast<uintptr_t>(slot), &ignored_branch)) {
            g_free_thunk_slots[i - 1] = g_free_thunk_slots[g_free_thunk_slot_count - 1];
            --g_free_thunk_slot_count;
            return slot;
        }
    }
    // 再从现有共享页 carving；同样先校验槽可达，再递增页内计数。
    for (size_t page_index = 0; page_index < g_thunk_page_count; ++page_index) {
        if (g_thunk_page_used[page_index] >= kSlotsPerPage) continue;
        const uintptr_t page = reinterpret_cast<uintptr_t>(g_thunk_pages[page_index]);
        const size_t offset = g_thunk_page_used[page_index] * kThunkSlot;
        if (offset > std::numeric_limits<uintptr_t>::max() - page) continue;
        void* slot = reinterpret_cast<void*>(page + offset);
        if (encode_branch(call_addr, reinterpret_cast<uintptr_t>(slot), &ignored_branch)) {
            ++g_thunk_page_used[page_index];
            ++g_thunk_slots_allocated;
            return slot;
        }
    }
    return nullptr;
}

void set_failure(AllocationFailure* failure, uintptr_t requested, uintptr_t actual,
                 const char* stage, int saved_errno) {
    if (failure == nullptr) return;
    failure->requested = requested;
    failure->actual = actual;
    failure->stage = stage;
    failure->saved_errno = saved_errno;
}

int unmap_invalid_region(void* region, AllocationFailure* failure, uintptr_t requested,
                         const char* stage, int saved_errno) {
    const int result = g_munmap_function(region, kPageSize);
    const int unmap_errno = result != 0 ? errno : 0;
    set_failure(failure, requested, reinterpret_cast<uintptr_t>(region),
                result != 0 ? "munmap_invalid_mapping" : stage,
                result != 0 ? unmap_errno : saved_errno);
    return result;
}

void* map_candidate(uintptr_t call_addr, uintptr_t candidate, AllocationFailure* failure) {
    const int base_flags = MAP_PRIVATE | MAP_ANONYMOUS;
    const int protection = PROT_READ | PROT_WRITE | PROT_EXEC;
    if (g_mapping_mode == MappingMode::kHint) {
        void* region = g_mmap_function(reinterpret_cast<void*>(candidate), kPageSize,
                                       protection, base_flags, -1, 0);
        const int saved_errno = region == MAP_FAILED ? errno : 0;
        if (region == MAP_FAILED) {
            set_failure(failure, candidate, 0, "mmap_hint", saved_errno);
            return nullptr;
        }
        const uintptr_t actual = reinterpret_cast<uintptr_t>(region);
        uint32_t ignored_branch = 0;
        if ((actual & (kPageSize - 1)) != 0) {
            (void)unmap_invalid_region(region, failure, candidate, "unaligned_hint_mapping", 0);
            return nullptr;
        }
        if (!encode_branch(call_addr, actual, &ignored_branch)) {
            (void)unmap_invalid_region(region, failure, candidate, "out_of_range_hint_mapping", 0);
            return nullptr;
        }
        return region;
    }

    void* region = g_mmap_function(reinterpret_cast<void*>(candidate), kPageSize, protection,
                                   base_flags | kMapFixedNoReplace, -1, 0);
    const int saved_errno = region == MAP_FAILED ? errno : 0;
    if (region == MAP_FAILED) {
        set_failure(failure, candidate, 0, "mmap_noreplace", saved_errno);
        if (saved_errno != EINVAL) return nullptr;

        // EINVAL may have another cause; ordinary hinted mmap is still safe because its actual
        // returned address is checked before the page can enter the pool.
        g_mapping_mode = MappingMode::kHint;
        return map_candidate(call_addr, candidate, failure);
    }

    const uintptr_t actual = reinterpret_cast<uintptr_t>(region);
    if (actual != candidate) {
        set_failure(failure, candidate, actual, "noreplace_address_mismatch", 0);
        (void)unmap_invalid_region(region, failure, candidate, "noreplace_address_mismatch", 0);
        g_mapping_mode = MappingMode::kHint;
        return map_candidate(call_addr, candidate, failure);
    }

    g_mapping_mode = MappingMode::kNoReplace;
    uint32_t ignored_branch = 0;
    if ((actual & (kPageSize - 1)) != 0) {
        (void)unmap_invalid_region(region, failure, candidate, "unaligned_noreplace_mapping", 0);
        return nullptr;
    }
    if (!encode_branch(call_addr, actual, &ignored_branch)) {
        (void)unmap_invalid_region(region, failure, candidate, "out_of_range_noreplace_mapping", 0);
        return nullptr;
    }
    return region;
}

// 从近址池取一个可用 thunk 槽（32B 对齐，不写内容）；调用方持锁。
void* acquire_thunk_slot_locked(uintptr_t call_addr, AllocationFailure* failure) {
    if (failure != nullptr) *failure = AllocationFailure{};
    if (void* slot = acquire_existing_thunk_slot_locked(call_addr); slot != nullptr) return slot;
    if (g_thunk_page_count >= g_thunk_pages.size()) {
        set_failure(failure, 0, 0, "page_pool_capacity", 0);
        return nullptr;
    }

    // 两级有界扫描：先 64KB 粗扫，失败后 4KB 细扫。候选地址始终用无符号边界算术。
    for (const bool coarse : {true, false}) {
        const uint64_t step = coarse ? 0x10000u : 0x1000u;
        const uintptr_t base = call_addr & ~(static_cast<uintptr_t>(kPageSize) - 1);
        for (uint64_t distance = step; distance < (uint64_t{1} << 27); distance += step) {
            for (const bool positive : {true, false}) {
                uintptr_t candidate = 0;
                if (!candidate_address(base, distance, positive, &candidate) ||
                    (candidate & (kPageSize - 1)) != 0) {
                    set_failure(failure, candidate, 0, "candidate_address_overflow", 0);
                    continue;
                }
                uint32_t ignored_branch = 0;
                if (!encode_branch(call_addr, candidate, &ignored_branch)) continue;

                void* region = map_candidate(call_addr, candidate, failure);
                if (region == nullptr) continue;
                const uintptr_t actual = reinterpret_cast<uintptr_t>(region);
                if (g_thunk_page_count >= g_thunk_pages.size()) {
                    (void)unmap_invalid_region(region, failure, candidate, "page_pool_capacity", 0);
                    return nullptr;
                }

                const size_t page_index = g_thunk_page_count++;
                g_thunk_pages[page_index] = region;
                g_thunk_page_used[page_index] = 1;
                ++g_thunk_slots_allocated;
                // map_candidate validated alignment and first-slot reachability before registration.
                return reinterpret_cast<void*>(actual);
            }
        }
    }
    return nullptr;
}

void log_thunk_allocation_failure(uintptr_t call_addr, void* wrapper,
                                  const AllocationFailure& failure) {
    static std::once_flag uname_once;
    static char kernel_release[sizeof(utsname{}.release)] = "unavailable";
    std::call_once(uname_once, [] {
        utsname system_info{};
        if (uname(&system_info) == 0) {
            std::snprintf(kernel_release, sizeof(kernel_release), "%s", system_info.release);
        }
    });

    char direction = '0';
    unsigned long long distance = 0;
    if (failure.actual != 0) {
        if (failure.actual >= call_addr) {
            direction = '+';
            distance = static_cast<unsigned long long>(failure.actual - call_addr);
        } else {
            direction = '-';
            distance = static_cast<unsigned long long>(call_addr - failure.actual);
        }
    }
    const char* mapping_mode = g_mapping_mode == MappingMode::kNoReplace
                                   ? "noreplace-observed"
                                   : (g_mapping_mode == MappingMode::kHint ? "hint-fallback"
                                                                           : "unknown");
    QOL_LOG_ERROR(QolDomain::kPlatform,
                  "thunk allocation failed call=%p wrapper=%p requested=%p actual=%p "
                  "delta=%c0x%llx stage=%s errno=%d mmap_mode=%s kernel=%s",
                  reinterpret_cast<void*>(call_addr), wrapper,
                  reinterpret_cast<void*>(failure.requested),
                  reinterpret_cast<void*>(failure.actual), direction, distance,
                  failure.stage, failure.saved_errno, mapping_mode, kernel_release);
}

bool write_code_word(uintptr_t addr, uint32_t word) {
    const uintptr_t page = addr & ~(static_cast<uintptr_t>(kPageSize) - 1);
    if (mprotect(reinterpret_cast<void*>(page), kPageSize,
                 PROT_READ | PROT_WRITE | PROT_EXEC) != 0) {
        QOL_LOG_ERROR(QolDomain::kPlatform, "mprotect failed addr=%p errno=%d",
                      reinterpret_cast<void*>(addr), errno);
        return false;
    }
    *reinterpret_cast<uint32_t*>(addr) = word;
    __builtin___clear_cache(reinterpret_cast<char*>(addr),
                            reinterpret_cast<char*>(addr + sizeof(uint32_t)));
    return true;
}

}  // namespace

#if defined(QOL_CALL_PATCH_TEST_SEAMS)
void call_patch_test_set_mapping_functions(CallPatchTestMmapFunction mmap_function,
                                           CallPatchTestMunmapFunction munmap_function) {
    std::lock_guard<std::mutex> lock(g_call_patch_mtx);
    g_mmap_function = mmap_function != nullptr ? mmap_function : &::mmap;
    g_munmap_function = munmap_function != nullptr ? munmap_function : &::munmap;
}

void call_patch_test_reset_state() {
    std::lock_guard<std::mutex> lock(g_call_patch_mtx);
    g_mapping_mode = MappingMode::kUnknown;
    g_mmap_function = &::mmap;
    g_munmap_function = &::munmap;
    g_thunk_pages.fill(nullptr);
    g_thunk_page_count = 0;
    g_thunk_page_used.fill(0);
    g_free_thunk_slots.fill(nullptr);
    g_free_thunk_slot_count = 0;
    g_thunk_slots_allocated = 0;
    g_patched_calls.fill(PatchedCall{});
    g_patched_call_count = 0;
}

bool call_patch_test_encode_branch(uintptr_t from, uintptr_t to, uint32_t* out) {
    return encode_branch(from, to, out);
}

bool call_patch_test_candidate_address(uintptr_t base, uint64_t distance, bool positive,
                                       uintptr_t* out_candidate) {
    return candidate_address(base, distance, positive, out_candidate);
}

void* call_patch_test_acquire_thunk_slot(uintptr_t call_addr) {
    std::lock_guard<std::mutex> lock(g_call_patch_mtx);
    AllocationFailure failure;
    return acquire_thunk_slot_locked(call_addr, &failure);
}

void* call_patch_test_acquire_existing_thunk_slot(uintptr_t call_addr) {
    std::lock_guard<std::mutex> lock(g_call_patch_mtx);
    return acquire_existing_thunk_slot_locked(call_addr);
}

void call_patch_test_release_thunk_slot(void* slot) {
    std::lock_guard<std::mutex> lock(g_call_patch_mtx);
    release_thunk_slot_locked(slot);
}
#endif

bool call_patch_install_bl(uintptr_t call_addr, uint32_t expected_word, void* wrapper) {
    if (call_addr == 0 || wrapper == nullptr) return false;
    std::lock_guard<std::mutex> lock(g_call_patch_mtx);

    // M-1 幂等：同一调用点 + 同一 wrapper 已安装成功，直接返回，避免重复分配 thunk。
    if (find_patched_locked(call_addr, wrapper)) return true;

    const uintptr_t wrapper_addr = reinterpret_cast<uintptr_t>(wrapper);
    const uint32_t current = *reinterpret_cast<const uint32_t*>(call_addr);
    if (current != expected_word) {
        QOL_LOG_ERROR(QolDomain::kPlatform,
                      "original word mismatch call=%p got=0x%08x expected=0x%08x",
                      reinterpret_cast<void*>(call_addr), current, expected_word);
        return false;
    }
    uintptr_t branch_target = wrapper_addr;
    void* thunk_slot = nullptr;
    uint32_t branch_word = 0;
    if (!encode_branch(call_addr, wrapper_addr, &branch_word)) {
        AllocationFailure failure;
        thunk_slot = acquire_thunk_slot_locked(call_addr, &failure);
        if (thunk_slot == nullptr) {
            log_thunk_allocation_failure(call_addr, wrapper, failure);
            return false;
        }
        branch_target = reinterpret_cast<uintptr_t>(thunk_slot);
        if (!encode_branch(call_addr, branch_target, &branch_word)) {
            release_thunk_slot_locked(thunk_slot);
            set_failure(&failure, branch_target, branch_target, "callsite_to_thunk_branch", 0);
            log_thunk_allocation_failure(call_addr, wrapper, failure);
            return false;
        }
        // All branch constraints are validated before the shared slot is written.
        emit_call_thunk(thunk_slot, wrapper_addr);
    }
    const uint32_t replacement = 0x94000000u | (branch_word & 0x03ffffffu);

    if (!write_code_word(call_addr, replacement)) {
        release_thunk_slot_locked(thunk_slot);
        return false;
    }
    if (!remember_patched_locked(call_addr, wrapper, replacement, current, branch_target, thunk_slot)) {
        (void)write_code_word(call_addr, expected_word);
        release_thunk_slot_locked(thunk_slot);
        return false;
    }
    QOL_LOG_INFO(QolDomain::kPlatform, "patched call=%p replacement=0x%08x",
                 reinterpret_cast<void*>(call_addr), replacement);
    return true;
}

bool call_patch_revert_bl(uintptr_t call_addr, uint32_t original_word) {
    if (call_addr == 0) return false;
    (void)original_word;
    std::lock_guard<std::mutex> lock(g_call_patch_mtx);

    uint32_t saved_word = 0;
    if (!saved_word_locked(call_addr, &saved_word)) {
        QOL_LOG_WARN(QolDomain::kPlatform, "call patch saved word missing call=%p",
                     reinterpret_cast<void*>(call_addr));
        return false;
    }

    const uint32_t current = *reinterpret_cast<const uint32_t*>(call_addr);
    if (current == saved_word) {  // 幂等：已还原
        forget_patched_locked(call_addr);
        return true;
    }
    if ((current & 0xFC000000u) != 0x94000000u) {
        QOL_LOG_ERROR(QolDomain::kPlatform,
                      "revert target is not a BL call call=%p current=0x%08x",
                      reinterpret_cast<void*>(call_addr), current);
        return false;  // fail-closed：不覆盖非 BL 指令
    }
    if (!write_code_word(call_addr, saved_word)) return false;
    forget_patched_locked(call_addr);
    QOL_LOG_INFO(QolDomain::kPlatform, "reverted call=%p original=0x%08x",
                 reinterpret_cast<void*>(call_addr), saved_word);
    return true;
}

bool call_patch_install_word(uintptr_t addr, uint32_t expected_word, uint32_t new_word) {
    if (addr == 0) return false;
    std::lock_guard<std::mutex> lock(g_call_patch_mtx);

    const uint32_t current = *reinterpret_cast<const uint32_t*>(addr);
    if (current == new_word) return true;  // 幂等：已安装
    if (current != expected_word) {
        QOL_LOG_ERROR(QolDomain::kPlatform,
                      "word patch mismatch addr=%p got=0x%08x expected=0x%08x",
                      reinterpret_cast<void*>(addr), current, expected_word);
        return false;  // fail-closed：不改写任何字节
    }
    if (!write_code_word(addr, new_word)) return false;
    QOL_LOG_INFO(QolDomain::kPlatform, "patched word addr=%p new=0x%08x",
                 reinterpret_cast<void*>(addr), new_word);
    return true;
}

bool call_patch_revert_word(uintptr_t addr, uint32_t original_word) {
    if (addr == 0) return false;
    std::lock_guard<std::mutex> lock(g_call_patch_mtx);

    const uint32_t current = *reinterpret_cast<const uint32_t*>(addr);
    if (current == original_word) return true;  // 幂等：已还原
    if (!write_code_word(addr, original_word)) return false;
    QOL_LOG_INFO(QolDomain::kPlatform, "reverted word addr=%p original=0x%08x",
                 reinterpret_cast<void*>(addr), original_word);
    return true;
}

uintptr_t call_patch_install_branch(uintptr_t call_addr, uint32_t expected_word, void* handler) {
    if (call_addr == 0 || handler == nullptr) return 0;
    std::lock_guard<std::mutex> lock(g_call_patch_mtx);

    // 幂等：同一站点 + 同一 handler 已安装成功 → 返回既有 thunk，不重复分配。
    uintptr_t existing = 0;
    if (find_patched_locked(call_addr, handler, &existing)) return existing;

    // fail-closed：先校验原字，避免为失败的安装分配（泄漏）thunk。
    const uint32_t current = *reinterpret_cast<const uint32_t*>(call_addr);
    if (current != expected_word) {
        QOL_LOG_ERROR(QolDomain::kPlatform,
                      "branch original word mismatch call=%p got=0x%08x expected=0x%08x",
                      reinterpret_cast<void*>(call_addr), current, expected_word);
        return 0;
    }
    AllocationFailure failure;
    void* thunk = acquire_thunk_slot_locked(call_addr, &failure);
    if (thunk == nullptr) {
        log_thunk_allocation_failure(call_addr, handler, failure);
        return 0;
    }
    const uintptr_t thunk_addr = reinterpret_cast<uintptr_t>(thunk);
    uint32_t replacement = 0;
    if (!encode_branch(call_addr, thunk_addr, &replacement)) {
        set_failure(&failure, thunk_addr, thunk_addr, "callsite_to_thunk_branch", 0);
        release_thunk_slot_locked(thunk);
        log_thunk_allocation_failure(call_addr, handler, failure);
        return 0;
    }
    emit_branch_thunk(thunk, reinterpret_cast<uintptr_t>(handler));
    if (!write_code_word(call_addr, replacement)) {
        release_thunk_slot_locked(thunk);
        return 0;
    }
    if (!remember_patched_locked(call_addr, handler, replacement, current, thunk_addr, thunk)) {
        (void)write_code_word(call_addr, expected_word);
        release_thunk_slot_locked(thunk);
        return 0;
    }
    QOL_LOG_INFO(QolDomain::kPlatform, "patched branch call=%p thunk=%p replacement=0x%08x",
                 reinterpret_cast<void*>(call_addr), reinterpret_cast<void*>(thunk_addr),
                 replacement);
    return thunk_addr;
}

bool call_patch_revert_branch(uintptr_t call_addr, uint32_t original_word) {
    if (call_addr == 0) return false;
    (void)original_word;
    std::lock_guard<std::mutex> lock(g_call_patch_mtx);

    uint32_t saved_word = 0;
    if (!saved_word_locked(call_addr, &saved_word)) {
        QOL_LOG_WARN(QolDomain::kPlatform, "branch patch saved word missing call=%p",
                     reinterpret_cast<void*>(call_addr));
        return false;
    }

    const uint32_t current = *reinterpret_cast<const uint32_t*>(call_addr);
    if (current == saved_word) {  // 幂等：已还原
        forget_patched_locked(call_addr);
        return true;
    }
    if ((current & 0xFC000000u) != 0x14000000u) {
        QOL_LOG_ERROR(QolDomain::kPlatform,
                      "revert target is not a B branch call=%p current=0x%08x",
                      reinterpret_cast<void*>(call_addr), current);
        return false;  // fail-closed：不覆盖非 B 指令
    }
    if (!write_code_word(call_addr, saved_word)) return false;
    forget_patched_locked(call_addr);
    QOL_LOG_INFO(QolDomain::kPlatform, "reverted branch call=%p original=0x%08x",
                 reinterpret_cast<void*>(call_addr), saved_word);
    return true;
}

bool call_patch_install_branch_with_return(uintptr_t call_addr, uint32_t expected_word,
                                           uint32_t alternate_expected_word, void* handler,
                                           uintptr_t return_addr, uint32_t tail_word) {
    if (call_addr == 0 || handler == nullptr || return_addr == 0 || tail_word == 0) return false;
    std::lock_guard<std::mutex> lock(g_call_patch_mtx);
    uintptr_t existing = 0;
    if (find_patched_locked(call_addr, handler, &existing)) return true;

    const uint32_t current = *reinterpret_cast<const uint32_t*>(call_addr);
    if (current != expected_word &&
        (alternate_expected_word == 0 || current != alternate_expected_word)) {
        QOL_LOG_ERROR(QolDomain::kPlatform,
                      "branch-return original word mismatch call=%p got=0x%08x expected=0x%08x alt=0x%08x",
                      reinterpret_cast<void*>(call_addr), current, expected_word,
                      alternate_expected_word);
        return false;
    }
    AllocationFailure failure;
    void* thunk = acquire_thunk_slot_locked(call_addr, &failure);
    if (thunk == nullptr) {
        log_thunk_allocation_failure(call_addr, handler, failure);
        return false;
    }
    uint32_t return_branch = 0;
    const uintptr_t thunk_addr = reinterpret_cast<uintptr_t>(thunk);
    uint32_t replacement = 0;
    if (!encode_branch(call_addr, thunk_addr, &replacement)) {
        set_failure(&failure, thunk_addr, thunk_addr, "callsite_to_thunk_branch", 0);
        release_thunk_slot_locked(thunk);
        log_thunk_allocation_failure(call_addr, handler, failure);
        return false;
    }
    if (thunk_addr > std::numeric_limits<uintptr_t>::max() - 12 ||
        !encode_branch(thunk_addr + 12, return_addr, &return_branch)) {
        set_failure(&failure, thunk_addr, return_addr, "thunk_return_branch", 0);
        release_thunk_slot_locked(thunk);
        log_thunk_allocation_failure(call_addr, handler, failure);
        return false;
    }
    // Both the callsite and return edges are checked before either thunk or callsite is written.
    emit_branch_return_thunk(thunk, reinterpret_cast<uintptr_t>(handler), return_branch, tail_word);
    if (!write_code_word(call_addr, replacement)) {
        release_thunk_slot_locked(thunk);
        return false;
    }
    if (!remember_patched_locked(call_addr, handler, replacement, current, thunk_addr, thunk)) {
        (void)write_code_word(call_addr, current);
        release_thunk_slot_locked(thunk);
        return false;
    }
    return true;
}

bool call_patch_revert_branch_with_return(uintptr_t call_addr, uint32_t expected_word,
                                          uint32_t alternate_expected_word) {
    if (call_addr == 0) return false;
    (void)expected_word;
    (void)alternate_expected_word;
    std::lock_guard<std::mutex> lock(g_call_patch_mtx);
    uint32_t saved_word = 0;
    if (!saved_word_locked(call_addr, &saved_word)) {
        QOL_LOG_WARN(QolDomain::kPlatform, "branch-return saved word missing call=%p",
                     reinterpret_cast<void*>(call_addr));
        return false;
    }
    const uint32_t current = *reinterpret_cast<const uint32_t*>(call_addr);
    if (current == saved_word) {
        forget_patched_locked(call_addr);
        return true;
    }
    if ((current & 0xFC000000u) != 0x14000000u) return false;
    if (!write_code_word(call_addr, saved_word)) return false;
    forget_patched_locked(call_addr);
    return true;
}

bool branch_return_install(uintptr_t call_addr, uint32_t expected_word,
                           uint32_t alternate_expected_word, void* handler,
                           uintptr_t return_addr, uint32_t tail_word) {
    return call_patch_install_branch_with_return(
        call_addr, expected_word, alternate_expected_word, handler, return_addr, tail_word);
}

bool branch_return_revert(uintptr_t call_addr, uint32_t expected_word,
                          uint32_t alternate_expected_word) {
    return call_patch_revert_branch_with_return(call_addr, expected_word,
                                                alternate_expected_word);
}

CallPatchStats call_patch_stats() {
    std::lock_guard<std::mutex> lock(g_call_patch_mtx);
    return CallPatchStats{static_cast<uint32_t>(g_thunk_page_count),
                          static_cast<uint32_t>(g_thunk_slots_allocated),
                          static_cast<uint32_t>(g_thunk_slots_allocated - g_free_thunk_slot_count),
                          static_cast<uint32_t>(g_free_thunk_slot_count),
                          static_cast<uint32_t>(g_patched_call_count)};
}
