// save_enter.cpp —— 进入存档（读档/新档）加载完成事件发源地。
//
// 发起点：SaveSlot_SlotButtonExe+0x88（读档）与 SelectCharacter_ButtonStartExe+0x10（新档）
// 的 BL，作为两条声明式 kCallPatch 资源（owner="core"）由宿主装配末段安装改指 wrapper；
// wrapper 先经 core.save 转换门禁并标记 pending，再复刻原调用。检测任务 save_enter_tick
// 由 infra::scheduler 注册在 kRenderPre 点位（仅 world 态 DrawPlay 执行），
// world 就绪后消费一次、publish kEventSaveEnter（旧回调设施已退役，消费者改为事件订阅）。
//
// P3c 重接：读档发起先经 core.save 的转换状态机 begin（prepare_detach 拒绝时阻断原版
// 转换），确认序（reset/ensure+journal 严格检查+新 epoch+session_ready+加载排空）由
// core.save 在 RenderPre 完成；save_enter_tick 只在确认序发布 ready 后才消费 pending、
// 发布 kEventSaveEnter。确认失败不消费、不派发、保持阻断。

#include "patch/save_enter.h"

#include "infra/scheduler.h"
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

namespace {

std::atomic<bool> g_pending{false};

std::atomic<bool> g_tick_registered{false};

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

// 该 patch 精确替换 SelectCharacter_ButtonStartExe+0x10 的原版 BL
// SelectCharacter_StartGame：SaveSlot_GoToNewGame 已创建 selection token 并进入选角页后，
// 此确认入口才启动原版新档流程。它不是带槽位的 begin_new adapter thunk；API 新档由
// save.cpp 经受控转换路径完成，两条入口不能互换。
void save_new_wrapper() {
    if (fn_select_character_start_game == nullptr) {
        QOL_LOG_WARN(QolDomain::kSave, "new game confirmation unavailable; original not invoked");
        return;
    }
    save_enter_mark_pending();
    fn_select_character_start_game();
}

// 两个发起点的声明式资源（owner="core"）：站点登记与原指令字见 game/access/patches.cpp
// （P_SAVESLOT_SLOT_BUTTON_EXE_RESUME / P_SELECTCHAR_BUTTON_START_EXE_STARTGAME，原字与
// save_enter_detail binding 同源）。resumable=true 表示安装失败后允许宿主声明式重装。
constexpr const char* kOwner = "core";
const qol::contracts::ResourceClaim kEnterClaims[] = {
    {
        "patch:SAVESLOT_SLOT_BUTTON_EXE+0x88",  // id
        "P_SAVESLOT_SLOT_BUTTON_EXE_RESUME",    // symbol：patch 点登记名
        qol::contracts::ResourceMechanism::kCallPatch,
        qol::contracts::ResourcePolicy::kPrivate,
        qol::contracts::ResourceRequirement::kRequired,
        qol::contracts::ResourceFailure::kFailClosed,
        true,   // resumable
        kOwner,
        "读档发起点：SaveSlot_SlotButtonExe 内联 bl 调用点，无函数指针槽可挂",
        nullptr,  // gate：常驻
        120,      // step_budget
        reinterpret_cast<void*>(&save_load_wrapper),  // handler（BL 目标）
    },
    {
        "patch:SELECTCHAR_BUTTON_START_EXE+0x10",
        "P_SELECTCHAR_BUTTON_START_EXE_STARTGAME",
        qol::contracts::ResourceMechanism::kCallPatch,
        qol::contracts::ResourcePolicy::kPrivate,
        qol::contracts::ResourceRequirement::kRequired,
        qol::contracts::ResourceFailure::kFailClosed,
        true,
        kOwner,
        "新档确认发起点：SelectCharacter_ButtonStartExe 内联 bl 调用点，无函数指针槽可挂",
        nullptr,
        120,
        reinterpret_cast<void*>(&save_new_wrapper),
    },
};

// 帧检测任务：core.save 确认序发布 ready 后消费 pending 并 publish kEventSaveEnter。
// should_fire 探针门禁是「core.save 确认未完成就不消费不派发」的顺序无关保证，必须保留。
// 确认未完成/失败：不消费 pending、不派发、保持阻断。
bool save_enter_tick(int64_t /*frame*/, void* /*ctx*/) {
    if (!save_enter_detail::should_fire(g_pending.load(std::memory_order_acquire),
                                        game_in_world())) {
        return true;
    }
    const auto& hooks = qol::contracts::qol_runtime_hooks();
    if (hooks.save_enter_ready != nullptr && !hooks.save_enter_ready()) return true;
    if (!g_pending.exchange(false, std::memory_order_acq_rel)) return true;  // 已被并发消费

    const qol::contracts::SaveEnterEvent event{current_save_slot()};
    (void)qol::infra::publish(qol::contracts::kEventSaveEnter, &event);
    // 由 g_pending.exchange 保证每次 pending 只触发一次（非逐帧），保持 INFO。
    QOL_LOG_INFO(QolDomain::kSave, "save enter fired");
    return true;
}

}  // namespace

void save_enter_mark_pending() {
    g_pending.store(true, std::memory_order_release);
}

bool save_enter_init() {
    if (g_tick_registered.exchange(true, std::memory_order_acq_rel)) return true;
    const qol::infra::FrameTaskId id = qol::infra::scheduler_add(
        qol::infra::FramePoint::kRenderPre, &save_enter_tick, nullptr, 1, 0);
    if (id == 0) {
        g_tick_registered.store(false, std::memory_order_release);  // 允许重试
        return false;
    }
    return true;
}

uint32_t save_enter_claim_count() {
    return sizeof(kEnterClaims) / sizeof(kEnterClaims[0]);
}

const qol::contracts::ResourceClaim* save_enter_claims() { return kEnterClaims; }
