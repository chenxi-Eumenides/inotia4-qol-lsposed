// save_exit.cpp —— 退出存档（world -> 主菜单）回调。
//
// 发起点：GAMESTATE_SetState 内 state==4 分支跳转表落点 +0xb0 的 `bl GAME_Exit` 经
// call_patch 改指 wrapper；wrapper 先派发回调（world 数据仍有效），再复刻原 GAME_Exit。
// GAME_Exit 在 .text 内仅此一个调用点，故仅覆盖 world -> 主菜单。

#include "core/native/save_exit.h"

#include "core/native/call_patch.h"
#include "core/native/qol_log.h"
#include "data/native/game_symbols.h"
#include "game_access.h"

#include <atomic>
#include <mutex>
#include <utility>
#include <vector>

namespace {

// GAMESTATE_SetState state==4 分支处原 `bl GAME_Exit` 指令字（llvm-objdump 核对）。
constexpr uint32_t kGameExitBlWord = 0x97febb3d;

std::mutex g_save_exit_mtx;
std::vector<std::pair<SaveExitFn, void*>> g_callbacks;  // 仅持锁访问

std::atomic<bool> g_host_installed{false};
std::atomic<bool> g_host_installing{false};

// 持锁快照回调列表，锁外逐个调用（回调在游戏主线程执行）。
void save_exit_fire() {
    std::vector<std::pair<SaveExitFn, void*>> snapshot;
    {
        std::lock_guard<std::mutex> lock(g_save_exit_mtx);
        snapshot = g_callbacks;
    }
    for (const auto& entry : snapshot) {
        entry.first(entry.second);
    }
    QOL_LOG_INFO(QolDomain::kSave, "save exit fired callbacks=%zu", snapshot.size());
}

// 先派发回调（保证执行且 world 数据仍有效），再复刻原 GAME_Exit。
void save_exit_wrapper() {
    save_exit_fire();
    if (fn_game_exit != nullptr) fn_game_exit();
}

}  // namespace

bool save_exit_register(SaveExitFn fn, void* ctx) {
    if (fn == nullptr) return false;
    std::lock_guard<std::mutex> lock(g_save_exit_mtx);
    for (const auto& entry : g_callbacks) {
        if (entry.first == fn && entry.second == ctx) return true;  // 幂等
    }
    g_callbacks.emplace_back(fn, ctx);
    return true;
}

void save_exit_unregister(SaveExitFn fn, void* ctx) {
    std::lock_guard<std::mutex> lock(g_save_exit_mtx);
    for (auto it = g_callbacks.begin(); it != g_callbacks.end();) {
        if (it->first == fn && it->second == ctx) {
            it = g_callbacks.erase(it);
        } else {
            ++it;
        }
    }
}

bool save_exit_host_install_if_ready() {
    if (g_host_installed.load(std::memory_order_acquire)) return true;
    // exchange 抢占安装权：并发重复调用时后到者直接返回当前状态（幂等）。
    if (g_host_installing.exchange(true, std::memory_order_acq_rel)) {
        return g_host_installed.load(std::memory_order_acquire);
    }

    if (!bridge_ready() || g_base == 0 || fn_game_exit == nullptr) {
        g_host_installing.store(false, std::memory_order_release);
        return false;  // 未就绪：不改写字节，允许后续重试
    }

    const uintptr_t call_addr = g_base +
        fn_resolve("F_GAMESTATE_SET_STATE_VMA", F_GAMESTATE_SET_STATE_VMA) +
        F_GAMESTATE_SET_STATE_GAME_EXIT_CALL_OFF;
    if (!call_patch_install_bl(call_addr, kGameExitBlWord,
                               reinterpret_cast<void*>(&save_exit_wrapper))) {
        QOL_LOG_ERROR(QolDomain::kSave,
                      "install failed call=%p expected=0x%08x",
                      reinterpret_cast<void*>(call_addr), kGameExitBlWord);
        g_host_installing.store(false, std::memory_order_release);
        return false;  // fail-closed：call_patch 已保证未改写任何字节
    }

    g_host_installed.store(true, std::memory_order_release);
    g_host_installing.store(false, std::memory_order_release);
    QOL_LOG_INFO(QolDomain::kSave, "installed call=%p",
                 reinterpret_cast<void*>(call_addr));
    return true;
}
