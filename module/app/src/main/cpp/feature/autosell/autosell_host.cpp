// autosell_host.cpp —— 自动出售主线程逐帧宿主安装（阶段 B）。

#include "feature/autosell/autosell_host.h"

#include "core/native/call_patch.h"
#include "core/native/frame_tick.h"
#include "data/native/game_symbols.h"
#include "feature/autosell/autosell_config.h"
#include "feature/autosell/autosell_scan.h"
#include "game_access.h"

#include <android/log.h>

#include <atomic>
#include <cstdint>

namespace {

constexpr char kTag[] = "Inotia4AutoSellHost";
// GAMESTATE_DrawPlay +0x74 处原 `bl GRPX_Start` 指令字（阶段 A 反汇编核对）。
constexpr uint32_t kDrawPlayGrpxCallWord = 0x97ffc6ef;

std::atomic<bool> g_installed{false};
uintptr_t g_call_addr = 0;

// 替换原 BL GRPX_Start：复刻原调用 + 派发逐帧宿主。
void autosell_draw_tail() {
    if (fn_grpx_start != nullptr) fn_grpx_start();
    frame_tick_dispatch();
}

}  // namespace

bool autosell_host_install_if_ready() {
    if (g_installed.load(std::memory_order_acquire)) return true;
    if (!bridge_ready()) return false;
    if (g_base == 0) return false;

    const uintptr_t call_addr = g_base +
        fn_resolve("F_GAMESTATE_DRAWPLAY_VMA", F_GAMESTATE_DRAWPLAY_VMA) +
        F_GAMESTATE_DRAWPLAY_GRPX_START_CALL_OFF;
    if (!call_patch_install_bl(call_addr, kDrawPlayGrpxCallWord,
                               reinterpret_cast<void*>(&autosell_draw_tail))) {
        __android_log_print(ANDROID_LOG_ERROR, kTag,
                            "install failed call=%p expected=0x%08x",
                            reinterpret_cast<void*>(call_addr), kDrawPlayGrpxCallWord);
        return false;  // fail-closed：call_patch 已保证未改写任何字节
    }

    g_call_addr = call_addr;
    autosell_init();
    autosell_set_host_installed(true);  // M-10：状态可查
    g_installed.store(true, std::memory_order_release);
    __android_log_print(ANDROID_LOG_INFO, kTag, "installed call=%p",
                        reinterpret_cast<void*>(call_addr));
    return true;
}

void autosell_host_shutdown() {
    if (!g_installed.exchange(false, std::memory_order_acq_rel)) return;
    autosell_set_host_installed(false);
    frame_tick_unregister(&autosell_tick, nullptr);
    if (g_call_addr != 0) {
        call_patch_revert_bl(g_call_addr, kDrawPlayGrpxCallWord);
        g_call_addr = 0;
    }
}
