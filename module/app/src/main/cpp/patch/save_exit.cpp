// save_exit.cpp —— 退出存档（world -> 主菜单）事件发源地。
//
// 发起点：GAMESTATE_SetState 内 state==4 分支跳转表落点 +0xb0 的 `bl GAME_Exit`，作为
// 声明式 kCallPatch 资源（owner="core"）由宿主装配末段安装改指 wrapper；handler 先
// publish kEventSaveExit（world 数据仍有效），再复刻原 GAME_Exit。GAME_Exit 在 .text 内
// 仅此一个调用点，故仅覆盖 world -> 主菜单。

#include "patch/save_exit.h"

#include "infra/log/qol_log.h"
#include "contracts/events.h"
#include "game/access/game_access.h"
#include "infra/events.h"

extern int current_save_slot();

namespace {

// 退出存档发起点 handler（kCallPatch claim 的 BL 目标）：先 publish kEventSaveExit
// （保证执行且 world 数据仍有效），再复刻原 GAME_Exit。派发顺序不得变（先事件后原函数）。
void save_exit_fire() {
    const qol::contracts::SaveExitEvent event{current_save_slot()};
    (void)qol::infra::publish(qol::contracts::kEventSaveExit, &event);
    QOL_LOG_INFO(QolDomain::kSave, "save exit fired");
    if (fn_game_exit != nullptr) fn_game_exit();
}

// 发起点声明（owner="core"）：站点登记见 game/access/patches.cpp
// （P_GAMESTATE_SET_STATE_GAME_EXIT，原指令字 0x97febb3d，llvm-objdump 核对）。
// resumable=true 表示安装失败后允许宿主声明式重装。
constexpr const char* kOwner = "core";
const qol::contracts::ResourceClaim kExitClaim{
    "patch:GAMESTATE_SET_STATE+0xb0",   // id
    "P_GAMESTATE_SET_STATE_GAME_EXIT",  // symbol：patch 点登记名
    qol::contracts::ResourceMechanism::kCallPatch,
    qol::contracts::ResourcePolicy::kPrivate,
    qol::contracts::ResourceRequirement::kRequired,
    qol::contracts::ResourceFailure::kFailClosed,
    true,  // resumable
    kOwner,
    "退出存档发起点：GAMESTATE_SetState state==4 分支内联 bl 调用点，无函数指针槽可挂",
    nullptr,  // gate：常驻
    120,      // step_budget
    reinterpret_cast<void*>(&save_exit_fire),  // handler（BL 目标）
};

}  // namespace

uint32_t save_exit_claim_count() { return 1; }

const qol::contracts::ResourceClaim* save_exit_claims() { return &kExitClaim; }
