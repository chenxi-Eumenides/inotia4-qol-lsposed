// frame_host.cpp —— 统一帧派发宿主锚点安装器（渲染开始前）。

#include "core/native/frame_host.h"

#include "core/native/call_patch.h"
#include "core/native/frame_task.h"
#include "core/native/qol_log.h"
#include "data/native/game_symbols.h"
#include "game_access.h"

#include <atomic>
#include <cstdint>

namespace {

// GAMESTATE_DrawPlay 内 +0x20 处原 `bl MAP_DrawBase` 指令字（llvm-objdump 核对）。
constexpr uint32_t kDrawPlayMapDrawBaseBlWord = 0x9401d18a;
// MainProcess 内 +0x40 处原 `bl STATE_NextStartProcess` 指令字（llvm-objdump 核对）。
constexpr uint32_t kMainProcessNextStateBlWord = 0x97ffff3d;

std::atomic<bool> g_frame_host_installed{false};
std::atomic<bool> g_frame_host_installing{false};

// 替换原 BL MAP_DrawBase：先派发渲染开始前帧任务，再复刻原调用。
void render_pre_wrapper() {
    frame_task_dispatch(kFramePointRenderPre);
    if (fn_map_drawbase != nullptr) fn_map_drawbase();
}

// 替换原 BL STATE_NextStartProcess：先派发逻辑帧开始前帧任务，再复刻原调用。
// 消费点位于帧计数自增与 Draw 之前；GAMESTATE_SetState 会清空旧态 Process/Draw 函数指针，
// 使本帧旧 world 处理被游戏自身的空指针门跳过（frame-dispatch-host.md §2）。
void logic_pre_wrapper() {
    frame_task_dispatch(kFramePointLogicPre);
    if (fn_state_next_start_process != nullptr) fn_state_next_start_process();
}

}  // namespace

bool frame_host_install_if_ready() {
    if (g_frame_host_installed.load(std::memory_order_acquire)) return true;
    // exchange 抢占安装权：并发重复调用时后到者直接返回当前状态（幂等）。
    if (g_frame_host_installing.exchange(true, std::memory_order_acq_rel)) {
        return g_frame_host_installed.load(std::memory_order_acquire);
    }

    if (!bridge_ready() || g_base == 0 || fn_map_drawbase == nullptr ||
        fn_state_next_start_process == nullptr) {
        g_frame_host_installing.store(false, std::memory_order_release);
        return false;  // 未就绪：不改写字节，允许后续重试
    }

    const uintptr_t call_addr = g_base +
        fn_resolve("F_GAMESTATE_DRAWPLAY_VMA", F_GAMESTATE_DRAWPLAY_VMA) +
        F_GAMESTATE_DRAWPLAY_DRAWBASE_CALL_OFF;
    if (!call_patch_install_bl(call_addr, kDrawPlayMapDrawBaseBlWord,
                               reinterpret_cast<void*>(&render_pre_wrapper))) {
        QOL_LOG_ERROR(QolDomain::kCore,
                      "install render failed call=%p expected=0x%08x",
                      reinterpret_cast<void*>(call_addr), kDrawPlayMapDrawBaseBlWord);
        g_frame_host_installing.store(false, std::memory_order_release);
        return false;  // fail-closed：call_patch 已保证未改写任何字节
    }

    const uintptr_t logic_addr = g_base +
        fn_resolve("F_MAINPROCESS_VMA", F_MAINPROCESS_VMA) +
        F_MAINPROCESS_NEXT_STATE_CALL_OFF;
    if (!call_patch_install_bl(logic_addr, kMainProcessNextStateBlWord,
                               reinterpret_cast<void*>(&logic_pre_wrapper))) {
        QOL_LOG_ERROR(QolDomain::kCore,
                      "install logic failed call=%p expected=0x%08x; rolling back render",
                      reinterpret_cast<void*>(logic_addr), kMainProcessNextStateBlWord);
        call_patch_revert_bl(call_addr, kDrawPlayMapDrawBaseBlWord);  // 全成或全退
        g_frame_host_installing.store(false, std::memory_order_release);
        return false;
    }

    g_frame_host_installed.store(true, std::memory_order_release);
    g_frame_host_installing.store(false, std::memory_order_release);
    QOL_LOG_INFO(QolDomain::kCore, "installed render=%p logic=%p",
                 reinterpret_cast<void*>(call_addr), reinterpret_cast<void*>(logic_addr));
    return true;
}

bool frame_host_anchors_installed() {
    return g_frame_host_installed.load(std::memory_order_acquire);
}
