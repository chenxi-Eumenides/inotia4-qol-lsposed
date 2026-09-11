#include "game_patch.h"

#include "game_access.h"
#include "game_inventory.h"
#include "game_state.h"
#include "data/native/game_symbols.h"
#include "game_ops_common.h"
#include "game_ptr_hook.h"
#include "stack_codec.h"
#include "feature/extension_bag/game_ui_virtbag.h"
#include "feature/patch/native_inventory_hook.h"
#include "feature/patch/inventory_hook_stage4.h"

#include <android/log.h>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <sys/mman.h>
#include <thread>
#include <unistd.h>

#define PATCH_TAG "Inotia4Export"
#define MOVE_TAG "Inotia4Move"
#define MOVE_LOG(...) __android_log_print(ANDROID_LOG_INFO, MOVE_TAG, __VA_ARGS__)

namespace {

std::recursive_mutex g_patch_mtx;
std::atomic<bool> g_stack_enabled{false};

// 合成器批量宝石合成 + 自定义 UI 按钮（v0.5.18）注入状态
std::mutex g_craft_mtx;            // 注入/还原互斥（串行化 enable/disable）
bool g_craft_injected = false;     // 是否已注入
void* g_craft_orig = nullptr;      // 原宝石按钮 ControlObject*（还原槽指针用）
PtrHook g_craft_exec_hook;         // 原宝石按钮 ExecuteProc 的函数指针 hook（覆盖为批量合成）
void* g_craft_mmap = nullptr;      // mmap 区域（ControlObject + 按钮数据）
size_t g_craft_mmap_len = 0;       // mmap 长度
std::atomic<bool> g_craft_want{false};           // 是否期望注入（配置开关）
std::atomic<bool> g_craft_thread_started{false};
PtrHook g_move_merge_hook;
std::mutex g_move_merge_mtx;
std::atomic<bool> g_move_merge_requested{false};
std::atomic<bool> g_extension_source_protection_requested{false};

uintptr_t patch_addr(const PatchEntry& e) {
    if (g_base == 0) return 0;
    return g_base + fn_resolve(e.func_macro, e.func_vma) + e.func_offset;
}

bool write_insn(uintptr_t addr, uint32_t value) {
    const uintptr_t page = addr & ~uintptr_t{0xFFF};
    const size_t plen = (addr + sizeof(uint32_t) - page + 0xFFF) & ~size_t{0xFFF};
    if (mprotect(reinterpret_cast<void*>(page), plen, PROT_READ | PROT_WRITE | PROT_EXEC) != 0) {
        __android_log_print(ANDROID_LOG_ERROR, PATCH_TAG, "patch: mprotect(0x%lx) failed errno=%d", static_cast<unsigned long>(page), errno);
        return false;
    }
    *reinterpret_cast<uint32_t*>(addr) = value;
    __builtin___clear_cache(reinterpret_cast<char*>(addr),
                            reinterpret_cast<char*>(addr + sizeof(uint32_t)));
    return true;
}

void write_back(const PatchEntry* entries, size_t upto) {
    for (size_t i = 0; i < upto; ++i) {
        uintptr_t addr = patch_addr(entries[i]);
        if (addr == 0) continue;
        write_insn(addr, entries[i].orig);
    }
}

}  // namespace

#include "game_patch_core.inc"
#include "game_patch_move_merge.inc"
#include "game_patch_craft.inc"
