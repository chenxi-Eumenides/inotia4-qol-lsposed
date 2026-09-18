// call_patch.cpp —— 通用 BL 调用点 patch（自动出售阶段 A 基础设施）。
//
// thunk/mprotect 逻辑源自扩展背包原 allocate_draw_thunk 的已验证实现（近址页 carving +
// 64KB 粗扫/4KB 细扫 + MAP_FIXED_NOREPLACE）；D2a-3b-2 后扩展背包改用本机制，
// 本文件为唯一实现。

#include "call_patch.h"

#include "core/native/qol_log.h"

#include <array>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <sys/mman.h>

namespace {

constexpr size_t kPageSize = 4096;
constexpr size_t kThunkSlot = 32;         // 每槽 32B 对齐（thunk 码 20B + 字面量 8B）
constexpr int64_t kBlRange = 0x08000000LL;  // BL 可达范围 ±128MB

std::mutex g_call_patch_mtx;

// thunk 页登记：每页 4096B 承载 32B thunk，共享复用；页必落在调用点 ±128MB 内。
std::array<void*, 8> g_thunk_pages{};
size_t g_thunk_page_count = 0;
size_t g_thunk_page_used = 0;

// 已安装调用点登记（M-1 幂等）：同一 call_addr + 同一 wrapper 重复 install 直接
// 返回 true，避免重复分配 thunk。容量上限仅用于避免无界增长；溢出时退化为原行为。
struct PatchedCall {
    uintptr_t call_addr;
    void* wrapper;
    uint32_t replacement;
    uintptr_t target;  // 实际写入的分支目标（BL：wrapper/thunk；B：thunk）
};
std::array<PatchedCall, 16> g_patched_calls{};
size_t g_patched_call_count = 0;

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

void remember_patched_locked(uintptr_t call_addr, void* wrapper, uint32_t replacement,
                             uintptr_t target) {
    if (find_patched_locked(call_addr, wrapper)) return;
    if (g_patched_call_count < g_patched_calls.size()) {
        g_patched_calls[g_patched_call_count++] = {call_addr, wrapper, replacement, target};
    }
}

void forget_patched_locked(uintptr_t call_addr) {
    for (size_t i = 0; i < g_patched_call_count;) {
        if (g_patched_calls[i].call_addr == call_addr) {
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

// 从近址池取一个可用 thunk 槽（32B 对齐，不写内容）；调用方持锁。
// 槽必落在 call_addr ±128MB 内，否则返回 nullptr。
void* acquire_thunk_slot_locked(uintptr_t call_addr) {
#ifndef MAP_FIXED_NOREPLACE
    (void)call_addr;
    return nullptr;
#else
    // 1) 已有页 carving（校验对本调用点的 BL/B 可达性）。
    if (g_thunk_page_count > 0) {
        void* page = g_thunk_pages[g_thunk_page_count - 1];
        void* slot = reinterpret_cast<uint8_t*>(page) + g_thunk_page_used * kThunkSlot;
        const int64_t reach = static_cast<int64_t>(reinterpret_cast<uintptr_t>(slot)) -
                              static_cast<int64_t>(call_addr);
        if ((g_thunk_page_used + 1) * kThunkSlot <= kPageSize && reach > -kBlRange &&
            reach < kBlRange) {
            ++g_thunk_page_used;
            return slot;
        }
    }
    // 2) 两级扫描新页：先 64KB 粗扫（快），失败后 4KB 细扫兜底。
    for (const int64_t coarse : {1, 0}) {
        const int64_t kStep = coarse ? 0x00010000 : 0x1000;
        const uintptr_t base = call_addr & ~(static_cast<uintptr_t>(kPageSize) - 1);
        for (int64_t distance = kStep; distance < kBlRange; distance += kStep) {
            for (int sign : {1, -1}) {
                const int64_t candidate = static_cast<int64_t>(base) + sign * distance;
                if (candidate <= 0) continue;
                void* region = mmap(reinterpret_cast<void*>(static_cast<uintptr_t>(candidate)),
                                    kPageSize, PROT_READ | PROT_WRITE | PROT_EXEC,
                                    MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
                if (region == MAP_FAILED) continue;
                if (g_thunk_page_count < g_thunk_pages.size()) {
                    g_thunk_pages[g_thunk_page_count++] = region;
                    g_thunk_page_used = 1;
                }
                return region;
            }
        }
    }
    return nullptr;
#endif
}

// BL thunk：取槽 + 写 call 型 thunk。
void* allocate_thunk_locked(uintptr_t call_addr, uintptr_t wrapper) {
    void* slot = acquire_thunk_slot_locked(call_addr);
    if (slot == nullptr) return nullptr;
    emit_call_thunk(slot, wrapper);
    return slot;
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

bool call_patch_install_bl(uintptr_t call_addr, uint32_t expected_word, void* wrapper) {
    if (call_addr == 0 || wrapper == nullptr) return false;
    std::lock_guard<std::mutex> lock(g_call_patch_mtx);

    // M-1 幂等：同一调用点 + 同一 wrapper 已安装成功，直接返回，避免重复分配 thunk。
    if (find_patched_locked(call_addr, wrapper)) return true;

    const uintptr_t wrapper_addr = reinterpret_cast<uintptr_t>(wrapper);
    const int64_t direct_delta = static_cast<int64_t>(wrapper_addr) - static_cast<int64_t>(call_addr);
    uintptr_t branch_target = wrapper_addr;
    if ((direct_delta & 0x3) != 0 || direct_delta <= -kBlRange || direct_delta >= kBlRange) {
        void* thunk = allocate_thunk_locked(call_addr, wrapper_addr);
        if (thunk == nullptr) {
            QOL_LOG_ERROR(QolDomain::kPlatform,
                          "thunk allocation failed call=%p wrapper=%p",
                          reinterpret_cast<void*>(call_addr), wrapper);
            return false;
        }
        branch_target = reinterpret_cast<uintptr_t>(thunk);
    }

    const int64_t delta = static_cast<int64_t>(branch_target) - static_cast<int64_t>(call_addr);
    if ((delta & 0x3) != 0 || delta <= -kBlRange || delta >= kBlRange) {
        QOL_LOG_ERROR(QolDomain::kPlatform,
                      "branch target out of BL range call=%p target=%p",
                      reinterpret_cast<void*>(call_addr), reinterpret_cast<void*>(branch_target));
        return false;
    }
    const uint32_t replacement =
        0x94000000u | (static_cast<uint32_t>(delta >> 2) & 0x03ffffffu);

    const uint32_t current = *reinterpret_cast<const uint32_t*>(call_addr);
    if (current == replacement) {  // 幂等：已安装
        remember_patched_locked(call_addr, wrapper, replacement, branch_target);
        return true;
    }
    if (current != expected_word) {
        QOL_LOG_ERROR(QolDomain::kPlatform,
                      "original word mismatch call=%p got=0x%08x expected=0x%08x",
                      reinterpret_cast<void*>(call_addr), current, expected_word);
        return false;  // fail-closed：不改写任何字节
    }
    if (!write_code_word(call_addr, replacement)) return false;
    remember_patched_locked(call_addr, wrapper, replacement, branch_target);
    QOL_LOG_INFO(QolDomain::kPlatform, "patched call=%p replacement=0x%08x",
                 reinterpret_cast<void*>(call_addr), replacement);
    return true;
}

bool call_patch_revert_bl(uintptr_t call_addr, uint32_t original_word) {
    if (call_addr == 0) return false;
    std::lock_guard<std::mutex> lock(g_call_patch_mtx);

    const uint32_t current = *reinterpret_cast<const uint32_t*>(call_addr);
    if (current == original_word) {  // 幂等：已还原
        forget_patched_locked(call_addr);
        return true;
    }
    if ((current & 0xFC000000u) != 0x94000000u) {
        QOL_LOG_ERROR(QolDomain::kPlatform,
                      "revert target is not a BL call call=%p current=0x%08x",
                      reinterpret_cast<void*>(call_addr), current);
        return false;  // fail-closed：不覆盖非 BL 指令
    }
    if (!write_code_word(call_addr, original_word)) return false;
    forget_patched_locked(call_addr);
    QOL_LOG_INFO(QolDomain::kPlatform, "reverted call=%p original=0x%08x",
                 reinterpret_cast<void*>(call_addr), original_word);
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
    void* thunk = acquire_thunk_slot_locked(call_addr);
    if (thunk == nullptr) {
        QOL_LOG_ERROR(QolDomain::kPlatform,
                      "branch target out of BL range call=%p handler=%p",
                      reinterpret_cast<void*>(call_addr), handler);
        return 0;
    }
    emit_branch_thunk(thunk, reinterpret_cast<uintptr_t>(handler));
    const uintptr_t thunk_addr = reinterpret_cast<uintptr_t>(thunk);
    const int64_t delta = static_cast<int64_t>(thunk_addr) - static_cast<int64_t>(call_addr);
    if ((delta & 0x3) != 0 || delta <= -kBlRange || delta >= kBlRange) {
        QOL_LOG_ERROR(QolDomain::kPlatform,
                      "branch target out of BL range call=%p thunk=%p",
                      reinterpret_cast<void*>(call_addr), reinterpret_cast<void*>(thunk_addr));
        return 0;
    }
    const uint32_t replacement =
        0x14000000u | (static_cast<uint32_t>(delta >> 2) & 0x03ffffffu);
    if (!write_code_word(call_addr, replacement)) return 0;
    remember_patched_locked(call_addr, handler, replacement, thunk_addr);
    QOL_LOG_INFO(QolDomain::kPlatform, "patched branch call=%p thunk=%p replacement=0x%08x",
                 reinterpret_cast<void*>(call_addr), reinterpret_cast<void*>(thunk_addr),
                 replacement);
    return thunk_addr;
}

bool call_patch_revert_branch(uintptr_t call_addr, uint32_t original_word) {
    if (call_addr == 0) return false;
    std::lock_guard<std::mutex> lock(g_call_patch_mtx);

    const uint32_t current = *reinterpret_cast<const uint32_t*>(call_addr);
    if (current == original_word) {  // 幂等：已还原
        forget_patched_locked(call_addr);
        return true;
    }
    if ((current & 0xFC000000u) != 0x14000000u) {
        QOL_LOG_ERROR(QolDomain::kPlatform,
                      "revert target is not a B branch call=%p current=0x%08x",
                      reinterpret_cast<void*>(call_addr), current);
        return false;  // fail-closed：不覆盖非 B 指令
    }
    if (!write_code_word(call_addr, original_word)) return false;
    forget_patched_locked(call_addr);
    QOL_LOG_INFO(QolDomain::kPlatform, "reverted branch call=%p original=0x%08x",
                 reinterpret_cast<void*>(call_addr), original_word);
    return true;
}
