#ifndef INOTIA4_QOL_NEAR_CODE_ALLOC_H
#define INOTIA4_QOL_NEAR_CODE_ALLOC_H

#include <cstddef>
#include <cstdint>
#include <memory>
#include <sys/types.h>

using NearMmapFn = void* (*)(void* context, void* address, size_t size, int protection,
                             int flags, int fd, off_t offset);
using NearMunmapFn = int (*)(void* context, void* address, size_t size);
using NearNowFn = uint64_t (*)(void* context);

constexpr uint64_t kNearCodeFailureBackoffMs = 1000;

struct NearAllocPort {
    void* context = nullptr;
    NearMmapFn mmap_fn = nullptr;
    NearMunmapFn munmap_fn = nullptr;
    bool use_fixed_noreplace = false;
    NearNowFn now_fn = nullptr;
};

class NearCodeAllocator {
public:
    explicit NearCodeAllocator(NearAllocPort port);
    ~NearCodeAllocator();

    NearCodeAllocator(const NearCodeAllocator&) = delete;
    NearCodeAllocator& operator=(const NearCodeAllocator&) = delete;

    void* page_alloc(uintptr_t target);
    void* slot_alloc(uintptr_t target, size_t size);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// 返回 target ±128MB 内的可执行页；失败返回 nullptr。
void* near_code_page_alloc(uintptr_t target);

// 在共享近址页中分配 32B 对齐的 thunk 槽；槽必须仍可由 BL 从 target 到达。
// near_code_page_alloc 分配的整页不参与槽复用。
void* near_code_slot_alloc(uintptr_t target, size_t size);

#endif
