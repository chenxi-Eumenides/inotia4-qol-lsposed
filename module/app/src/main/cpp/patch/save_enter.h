#pragma once

#include <cstdint>

#include "contracts/plugin.h"

// 进入存档（读档/新档）加载完成事件发源地。
//
// 语义：仅在「读档 / 新档」加载完成、进入 world 后触发一次；切图返回不触发（切图不经过
// 两个发起点）。两个发起点是声明式 kCallPatch 资源（owner="core"，见 save_enter_claims），
// 由宿主装配末段登记并经机制分派安装；wrapper 先经 core.save 转换门禁、标记 pending，
// 再复刻原调用。检测任务 save_enter_tick 由 infra::scheduler 注册在 kRenderPre（仅 world
// 态 DrawPlay 执行），core.save 确认序发布 ready 后消费一次并 publish kEventSaveEnter；
// 确认未完成/失败不消费、不派发、保持阻断。
//
// 线程模型：mark_pending 任意线程可调；tick 与事件派发在游戏主线程执行。
// 本头文件纯逻辑、不依赖 Android，供 host 测试引用。

// 标记「进入存档流程已发起」；任意线程可调。
void save_enter_mark_pending();

// 初始化检测任务（幂等）：向 infra::scheduler 注册 save_enter_tick（kRenderPre）。
// 失败重置注册标记，允许重试。
bool save_enter_init();

// 两个发起点的声明式资源表（owner="core"，kCallPatch；handler 为本 TU 的 wrapper）。
// 只声明不安装：宿主装配末段经 hooks 登记 + 机制分派安装；未就绪时不改写字节，可重试。
uint32_t save_enter_claim_count();
const qol::contracts::ResourceClaim* save_enter_claims();

namespace save_enter_detail {

struct SaveEnterPatchBinding {
    const char* callsite_symbol;
    const char* target_symbol;
    uint32_t original_call;
};

inline constexpr SaveEnterPatchBinding kResumeBinding{
    "F_SAVESLOT_SLOT_BUTTON_EXE_VMA", "F_GAME_START_RESUME_GAME_VMA", 0x97fecd56};
inline constexpr SaveEnterPatchBinding kNewGameConfirmBinding{
    "F_SELECTCHAR_BUTTON_START_EXE_VMA", "F_SELECT_CHARACTER_START_GAME_VMA", 0x97ffffea};

// 触发判定纯函数：pending 且已进入 world 才触发。
constexpr bool should_fire(bool pending, bool in_world) {
    return pending && in_world;
}

}  // namespace save_enter_detail
