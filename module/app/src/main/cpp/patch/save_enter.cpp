// save_enter.cpp —— 进入存档（读档/新档）加载完成回调。
//
// 发起点：SaveSlot_SlotButtonExe+0x88（读档）与 SelectCharacter_ButtonStartExe+0x10（新档）
// 的 BL 经 call_patch 改指 wrapper；wrapper 先标记 pending 再复刻原调用。检测任务由
// frame_task 注册在 kFramePointRenderPre（仅 world 态 DrawPlay 执行），world 就绪后触发一次。
//
// P3c 重接：读档发起先经 core.save 的转换状态机 begin（prepare_detach 拒绝时阻断原版
// 转换），确认序（reset/ensure+journal 严格检查+新 epoch+session_ready+加载排空）由
// core.save 在 RenderPre 完成；save_enter_tick 只在确认序发布 ready 后才消费 pending、
// 派发旧回调与 kEventSaveEnter。确认失败不消费、不派发、保持阻断。

#include "patch/save_enter.h"

#include "patch/call_patch.h"
#include "infra/frame/frame_task.h"
#include "infra/log/qol_log.h"
#include "contracts/events.h"
#include "contracts/runtime_hooks.h"
#include "contracts/save_route.h"
#include "contracts/game_ports/save_transaction_port.h"
#include "game/access/game_access.h"
#include "game/access/save.h"
#include "game/access/save_transaction.h"
#include "game/state/game_state.h"
#include "infra/events.h"

#include <atomic>
#include <mutex>
#include <utility>
#include <vector>

namespace {

// 发起点原指令字（llvm-objdump 核对）。
constexpr uint32_t kResumeBlWord = 0x97fecd56;    // SaveSlot_SlotButtonExe+0x88 bl GAME_StartResumeGame
constexpr uint32_t kStartGameBlWord = 0x97ffffea; // SelectCharacter_ButtonStartExe+0x10 bl SelectCharacter_StartGame

std::mutex g_save_enter_mtx;
std::vector<std::pair<SaveEnterFn, void*>> g_callbacks;  // 仅持锁访问

std::atomic<bool> g_pending{false};

std::atomic<bool> g_tick_registered{false};
std::atomic<bool> g_host_installed{false};
std::atomic<bool> g_host_installing{false};

// 读档 wrapper：先经 core.save begin 转换（kResume；准入关闭时阻断原版转换），成功后
// 登记 loading 旁路 invocation 与未就绪转换（SAVE_Save 的读档期旁路矩阵输入），再标记
// pending 并复刻原调用。原版转换返回后旁路窗口关闭：SAVE_LoadCharacterAll 内部的
// SAVE_Save 只可能发生在该窗口内，其后的 SAVE_Save 无 loading 语义（session 未 ready
// 时将被矩阵拒绝，fail-closed）。
void save_load_wrapper(int32_t slot) {
    uint64_t transition = 0;
    const auto& hooks = qol::contracts::qol_runtime_hooks();
    const bool began = hooks.save_transition_begin_thunk != nullptr &&
                       hooks.save_transition_begin_thunk(0, slot, &transition);
    if (!began) {
        QOL_LOG_WARN(QolDomain::kSave,
                     "save enter blocked by transition gate slot=%d", slot);
        return;  // 不消费、不执行原版转换：模块准入失败即阻断（fail-closed）。
    }
    // API 与按钮读档共用受控转换上下文（统一设置未就绪 transition + invocation）。
    const uint64_t bypass = qol::game::save_load_bypass_begin(
        transition, slot, static_cast<uint32_t>(qol::contracts::QolSaveCallKind::kResume));
    if (bypass == 0) {
        const auto& hooks_cancel = qol::contracts::qol_runtime_hooks();
        if (hooks_cancel.save_transition_cancel_thunk != nullptr)
            hooks_cancel.save_transition_cancel_thunk(transition);
        QOL_LOG_WARN(QolDomain::kSave,
                     "save enter blocked because loading bypass scope is occupied slot=%d", slot);
        return;
    }
    save_enter_mark_pending();
    if (fn_game_start_resume_game != nullptr) {
        fn_game_start_resume_game(slot);
    } else {
        // 原版未启动：显式取消未决转换（释放 token/lease），并结束 bypass 作用域。
        const auto& hooks_cancel = qol::contracts::qol_runtime_hooks();
        if (hooks_cancel.save_transition_cancel_thunk != nullptr)
            hooks_cancel.save_transition_cancel_thunk(transition);
        qol::game::save_load_bypass_end(bypass);
        return;
    }
    qol::game::save_load_bypass_end(bypass);
}

// 新档 wrapper：selection token 已由 SaveSlot_GoToNewGame 入口 hook begin（core.save），
// 确认按钮只标记 pending 并复刻原调用；确认序在 core.save RenderPre 按 token id 复用。
void save_new_wrapper() {
    save_enter_mark_pending();
    if (fn_select_character_start_game != nullptr) fn_select_character_start_game();
}

// 帧检测任务：core.save 确认序发布 ready 后消费一次并锁外派发回调（回调在主线程执行）。
// 确认未完成/失败：不消费 pending、不派发、保持阻断。
bool save_enter_tick(int64_t /*frame*/, void* /*ctx*/) {
    if (!save_enter_detail::should_fire(g_pending.load(std::memory_order_acquire),
                                        game_in_world())) {
        return true;
    }
    const auto& hooks = qol::contracts::qol_runtime_hooks();
    if (hooks.save_enter_ready != nullptr && !hooks.save_enter_ready()) return true;
    if (!g_pending.exchange(false, std::memory_order_acq_rel)) return true;  // 已被并发消费

    std::vector<std::pair<SaveEnterFn, void*>> snapshot;
    {
        std::lock_guard<std::mutex> lock(g_save_enter_mtx);
        snapshot = g_callbacks;  // 持锁快照，锁外执行回调
    }
    for (const auto& entry : snapshot) {
        entry.first(entry.second);
    }
    const qol::contracts::SaveEnterEvent event{current_save_slot()};
    (void)qol::infra::publish(qol::contracts::kEventSaveEnter, &event);
    // 由 g_pending.exchange 保证每次 pending 只触发一次（非逐帧），保持 INFO。
    QOL_LOG_INFO(QolDomain::kSave, "save enter fired callbacks=%zu", snapshot.size());
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

    const qol::game::SaveSiteRef load_site = qol::game::save_enter_load_site();
    const uintptr_t load_addr =
        g_base + fn_resolve("F_SAVESLOT_SLOT_BUTTON_EXE_VMA", load_site.vma) +
        load_site.call_off;
    if (!call_patch_install_bl(load_addr, kResumeBlWord,
                               reinterpret_cast<void*>(&save_load_wrapper))) {
        QOL_LOG_ERROR(QolDomain::kSave,
                      "install load-site failed call=%p expected=0x%08x",
                      reinterpret_cast<void*>(load_addr), kResumeBlWord);
        g_host_installing.store(false, std::memory_order_release);
        return false;  // fail-closed：call_patch 已保证未改写任何字节
    }

    const qol::game::SaveSiteRef new_site = qol::game::save_enter_new_site();
    const uintptr_t new_addr =
        g_base + fn_resolve("F_SELECTCHAR_BUTTON_START_EXE_VMA", new_site.vma) +
        new_site.call_off;
    if (!call_patch_install_bl(new_addr, kStartGameBlWord,
                               reinterpret_cast<void*>(&save_new_wrapper))) {
        QOL_LOG_ERROR(QolDomain::kSave,
                      "install new-site failed call=%p; rolling back load-site",
                      reinterpret_cast<void*>(new_addr));
        call_patch_revert_bl(load_addr, kResumeBlWord);  // 全成或全退
        g_host_installing.store(false, std::memory_order_release);
        return false;
    }

    g_host_installed.store(true, std::memory_order_release);
    g_host_installing.store(false, std::memory_order_release);
    QOL_LOG_INFO(QolDomain::kSave, "installed load=%p new=%p",
                 reinterpret_cast<void*>(load_addr), reinterpret_cast<void*>(new_addr));
    return true;
}
