#include "core/native/near_code_alloc.h"

/*
 * A failed complete scan is bounded at 4,094 coarse candidates plus 61,440 fine
 * candidates after the fine sweep skips coarse offsets. Each candidate uses at
 * most one mmap and, on an unexpected return address, one munmap. The failure
 * path is intentionally exhaustive so a free page visible only at 4KB
 * granularity is not missed.
 */
#include "core/native/near_thunk_rules.h"
#include "core/native/qol_log.h"

#include <array>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <sys/mman.h>
#include <vector>

namespace {

struct CodePage {
    uintptr_t address = 0;
    size_t used = 0;
};

void* system_mmap(void*, void* address, size_t size, int protection, int flags, int fd,
                  off_t offset) {
    return mmap(address, size, protection, flags, fd, offset);
}

int system_munmap(void*, void* address, size_t size) {
    return munmap(address, size);
}

uint64_t system_now(void*) {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                     std::chrono::steady_clock::now().time_since_epoch())
                                     .count());
}

NearAllocPort system_port() {
    return {nullptr, system_mmap, system_munmap, false, system_now};
}

bool page_is_reachable(uintptr_t target, uintptr_t page) {
    return near_thunk_rules::bl_reachable(target, page);
}

bool slot_is_reachable(uintptr_t target, uintptr_t slot) {
    return near_thunk_rules::branch_reachable(target, slot);
}

void log_page_failure(uintptr_t target) {
    QOL_LOG_ERROR(QolDomain::kPlatform,
                  "near code allocation failed target=%p strategy=noreplace-or-hint-validated",
                  reinterpret_cast<void*>(target));
}

void log_slot_failure(uintptr_t target, size_t size) {
    QOL_LOG_ERROR(QolDomain::kPlatform,
                  "near code slot allocation failed target=%p size=%zu strategy=noreplace-or-hint-validated",
                  reinterpret_cast<void*>(target), size);
}

}  // namespace

struct NearCodeAllocator::Impl {
    explicit Impl(NearAllocPort allocator_port) : port(allocator_port) {}

    NearAllocPort port;
    std::mutex mutex;
    std::vector<CodePage> pages;

    struct FailureRecord {
        uintptr_t target = 0;
        uint64_t failed_at_ms = 0;
        bool valid = false;
    };

    static constexpr size_t kFailureRecordCount = 64;
    std::array<FailureRecord, kFailureRecordCount> failures{};

    // 当前 3 个使用方、十余个调用点远少于 64 条记录，不会因多目标轮换挤掉记录；记录在 1000ms 后可重试，成功后清理。
    uint64_t now_ms() const {
        return port.now_fn == nullptr ? system_now(port.context) : port.now_fn(port.context);
    }

    bool failure_backoff_active(uintptr_t target) const {
        const uint64_t now = now_ms();
        for (const FailureRecord& failure : failures) {
            if (failure.valid && failure.target == target && now >= failure.failed_at_ms &&
                now - failure.failed_at_ms < kNearCodeFailureBackoffMs) {
                return true;
            }
        }
        return false;
    }

    void clear_failure(uintptr_t target) {
        for (FailureRecord& failure : failures) {
            if (failure.valid && failure.target == target) failure.valid = false;
        }
    }

    void remember_failure(uintptr_t target) {
        const uint64_t now = now_ms();
        FailureRecord* replacement = nullptr;
        for (FailureRecord& failure : failures) {
            if (failure.valid && failure.target == target) {
                failure.failed_at_ms = now;
                return;
            }
            if (!failure.valid && replacement == nullptr) replacement = &failure;
        }
        if (replacement == nullptr) {
            replacement = &failures[0];
            for (FailureRecord& failure : failures) {
                if (failure.failed_at_ms < replacement->failed_at_ms) replacement = &failure;
            }
        }
        *replacement = {target, now, true};
    }

    bool release_unexpected(void* region) {
        if (region == MAP_FAILED) return true;
        if (port.munmap_fn == nullptr) return false;
        return port.munmap_fn(port.context, region, near_thunk_rules::kPageSize) == 0;
    }

    void* map_once(uintptr_t candidate, int flags) {
        if (port.mmap_fn == nullptr) return MAP_FAILED;
        void* region = port.mmap_fn(port.context, reinterpret_cast<void*>(candidate),
                                    near_thunk_rules::kPageSize,
                                    PROT_READ | PROT_WRITE | PROT_EXEC, flags, -1, 0);
        if (region == reinterpret_cast<void*>(candidate)) return region;
        if (region != MAP_FAILED) (void)release_unexpected(region);
        return MAP_FAILED;
    }

    bool map_candidate(uintptr_t candidate) {
        int flags = MAP_PRIVATE | MAP_ANONYMOUS;
#ifdef MAP_FIXED_NOREPLACE
        if (port.use_fixed_noreplace) flags |= MAP_FIXED_NOREPLACE;
#endif
        return map_once(candidate, flags) != MAP_FAILED;
    }

    bool try_map_page(uintptr_t target, uintptr_t* out_page) {
        if (out_page == nullptr) return false;
        const std::vector<uintptr_t> candidates = near_thunk_rules::enumerate_candidates(target);
        for (const uintptr_t candidate : candidates) {
            if (map_candidate(candidate)) {
                *out_page = candidate;
                return true;
            }
        }
        return false;
    }

    void* page_alloc(uintptr_t target) {
        if (target == 0) return nullptr;
        std::lock_guard<std::mutex> lock(mutex);
        if (failure_backoff_active(target)) return nullptr;
        uintptr_t page = 0;
        if (!try_map_page(target, &page) || !page_is_reachable(target, page)) {
            if (page != 0 && port.munmap_fn != nullptr) {
                (void)port.munmap_fn(port.context, reinterpret_cast<void*>(page),
                                     near_thunk_rules::kPageSize);
            }
            remember_failure(target);
            log_page_failure(target);
            return nullptr;
        }
        clear_failure(target);
        return reinterpret_cast<void*>(page);
    }

    void* slot_alloc(uintptr_t target, size_t size) {
        if (target == 0 || size == 0 || size > near_thunk_rules::kPageSize ||
            size % near_thunk_rules::kThunkSlotSize != 0) {
            return nullptr;
        }
        std::lock_guard<std::mutex> lock(mutex);

        for (CodePage& page : pages) {
            const size_t aligned_used =
                (page.used + near_thunk_rules::kThunkSlotSize - 1) &
                ~(near_thunk_rules::kThunkSlotSize - 1);
            if (aligned_used > near_thunk_rules::kPageSize ||
                size > near_thunk_rules::kPageSize - aligned_used) {
                continue;
            }
            const uintptr_t slot = page.address + aligned_used;
            if (!slot_is_reachable(target, slot)) continue;
            page.used = aligned_used + size;
            clear_failure(target);
            return reinterpret_cast<void*>(slot);
        }

        if (failure_backoff_active(target)) return nullptr;
        uintptr_t page = 0;
        if (!try_map_page(target, &page) || !page_is_reachable(target, page)) {
            if (page != 0 && port.munmap_fn != nullptr) {
                (void)port.munmap_fn(port.context, reinterpret_cast<void*>(page),
                                     near_thunk_rules::kPageSize);
            }
            remember_failure(target);
            log_slot_failure(target, size);
            return nullptr;
        }
        clear_failure(target);
        pages.push_back({page, size});
        return reinterpret_cast<void*>(page);
    }
};

NearCodeAllocator::NearCodeAllocator(NearAllocPort port)
    : impl_(std::make_unique<Impl>(port)) {}

NearCodeAllocator::~NearCodeAllocator() = default;

void* NearCodeAllocator::page_alloc(uintptr_t target) {
    return impl_->page_alloc(target);
}

void* NearCodeAllocator::slot_alloc(uintptr_t target, size_t size) {
    return impl_->slot_alloc(target, size);
}

namespace {

NearCodeAllocator& default_allocator() {
    static NearCodeAllocator allocator(system_port());
    return allocator;
}

}  // namespace

void* near_code_page_alloc(uintptr_t target) {
    void* page = default_allocator().page_alloc(target);
    return page;
}

void* near_code_slot_alloc(uintptr_t target, size_t size) {
    void* slot = default_allocator().slot_alloc(target, size);
    return slot;
}
