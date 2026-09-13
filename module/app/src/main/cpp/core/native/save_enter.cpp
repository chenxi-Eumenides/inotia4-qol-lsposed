// save_enter.cpp —— 进入存档（读档/新档）加载完成回调。
//
// 发起点：SaveSlot_SlotButtonExe+0x88（读档）与 SelectCharacter_ButtonStartExe+0x10（新档）
// 的 BL 经 call_patch 改指 wrapper；wrapper 先标记 pending 再复刻原调用。检测任务由
// frame_task 注册在 kFramePointRenderPre（仅 world 态 DrawPlay 执行），world 就绪后触发一次。

#include "core/native/save_enter.h"

#include "core/native/call_patch.h"
#include "core/native/frame_task.h"
#include "data/native/game_symbols.h"
#include "game_access.h"
#include "game_state.h"

#include <android/log.h>

#include <atomic>
#include <mutex>
#include <utility>
#include <vector>

namespace {

constexpr char kTag[] = "Inotia4SaveEnter";
// 发起点原指令字（llvm-objdump 核对）。
constexpr uint32_t kResumeBlWord = 0x97fecd56;    // SaveSlot_SlotButtonExe+0x88 bl GAME_StartResumeGame
constexpr uint32_t kStartGameBlWord = 0x97ffffea; // SelectCharacter_ButtonStartExe+0x10 bl SelectCharacter_StartGame

std::mutex g_save_enter_mtx;
std::vector<std::pair<SaveEnterFn, void*>> g_callbacks;  // 仅持锁访问

std::atomic<bool> g_pending{false};

std::atomic<bool> g_tick_registered{false};
std::atomic<bool> g_host_installed{false};
std::atomic<bool> g_host_installing{false};

// 读档 wrapper：标记 pending 后复刻原调用。
void save_load_wrapper(int32_t slot) {
    save_enter_mark_pending();
    if (fn_game_start_resume_game != nullptr) fn_game_start_resume_game(slot);
}

// 新档 wrapper：标记 pending 后复刻原调用。
void save_new_wrapper() {
    save_enter_mark_pending();
    if (fn_select_character_start_game != nullptr) fn_select_character_start_game();
}

// 帧检测任务：pending 且在 world 时消费一次并锁外派发回调（回调在主线程执行）。
bool save_enter_tick(int64_t /*frame*/, void* /*ctx*/) {
    if (!save_enter_detail::should_fire(g_pending.load(std::memory_order_acquire),
                                        game_in_world())) {
        return true;
    }
    if (!g_pending.exchange(false, std::memory_order_acq_rel)) return true;  // 已被并发消费

    std::vector<std::pair<SaveEnterFn, void*>> snapshot;
    {
        std::lock_guard<std::mutex> lock(g_save_enter_mtx);
        snapshot = g_callbacks;  // 持锁快照，锁外执行回调
    }
    for (const auto& entry : snapshot) {
        entry.first(entry.second);
    }
    __android_log_print(ANDROID_LOG_INFO, kTag, "save enter fired callbacks=%zu", snapshot.size());
    return true;
}

}  // namespace

bool save_enter_register(SaveEnterFn fn, void* ctx) {
    if (fn == nullptr) return false;
    std::lock_guard<std::mutex> lock(g_save_enter_mtx);
    for (const auto& entry : g_callbacks) {
        if (entry.first == fn && entry.second == ctx) return true;  // 幂等
    }
    g_callbacks.emplace_back(fn, ctx);
    return true;
}

void save_enter_unregister(SaveEnterFn fn, void* ctx) {
    std::lock_guard<std::mutex> lock(g_save_enter_mtx);
    for (auto it = g_callbacks.begin(); it != g_callbacks.end();) {
        if (it->first == fn && it->second == ctx) {
            it = g_callbacks.erase(it);
        } else {
            ++it;
        }
    }
}

void save_enter_mark_pending() {
    g_pending.store(true, std::memory_order_release);
}

bool save_enter_init() {
    if (g_tick_registered.exchange(true, std::memory_order_acq_rel)) return true;
    const FrameTaskId id = frame_task_add(kFramePointRenderPre, &save_enter_tick, nullptr, 0, 0);
    if (id == 0) {
        g_tick_registered.store(false, std::memory_order_release);  // 允许重试
        return false;
    }
    return true;
}

bool save_enter_host_install_if_ready() {
    if (g_host_installed.load(std::memory_order_acquire)) return true;
    // exchange 抢占安装权：并发重复调用时后到者直接返回当前状态（幂等）。
    if (g_host_installing.exchange(true, std::memory_order_acq_rel)) {
        return g_host_installed.load(std::memory_order_acquire);
    }

    if (!bridge_ready() || g_base == 0 || fn_game_start_resume_game == nullptr ||
        fn_select_character_start_game == nullptr) {
        g_host_installing.store(false, std::memory_order_release);
        return false;  // 未就绪：不改写字节，允许后续重试
    }

    const uintptr_t load_addr = g_base +
        fn_resolve("F_SAVESLOT_SLOT_BUTTON_EXE_VMA", F_SAVESLOT_SLOT_BUTTON_EXE_VMA) +
        F_SAVESLOT_SLOT_BUTTON_EXE_RESUME_CALL_OFF;
    if (!call_patch_install_bl(load_addr, kResumeBlWord,
                               reinterpret_cast<void*>(&save_load_wrapper))) {
        __android_log_print(ANDROID_LOG_ERROR, kTag,
                            "install load-site failed call=%p expected=0x%08x",
                            reinterpret_cast<void*>(load_addr), kResumeBlWord);
        g_host_installing.store(false, std::memory_order_release);
        return false;  // fail-closed：call_patch 已保证未改写任何字节
    }

    const uintptr_t new_addr = g_base +
        fn_resolve("F_SELECTCHAR_BUTTON_START_EXE_VMA", F_SELECTCHAR_BUTTON_START_EXE_VMA) +
        F_SELECTCHAR_BUTTON_START_EXE_STARTGAME_CALL_OFF;
    if (!call_patch_install_bl(new_addr, kStartGameBlWord,
                               reinterpret_cast<void*>(&save_new_wrapper))) {
        __android_log_print(ANDROID_LOG_ERROR, kTag,
                            "install new-site failed call=%p; rolling back load-site",
                            reinterpret_cast<void*>(new_addr));
        call_patch_revert_bl(load_addr, kResumeBlWord);  // 全成或全退
        g_host_installing.store(false, std::memory_order_release);
        return false;
    }

    g_host_installed.store(true, std::memory_order_release);
    g_host_installing.store(false, std::memory_order_release);
    __android_log_print(ANDROID_LOG_INFO, kTag, "installed load=%p new=%p",
                        reinterpret_cast<void*>(load_addr), reinterpret_cast<void*>(new_addr));
    return true;
}
