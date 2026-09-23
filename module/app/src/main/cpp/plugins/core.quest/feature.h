#pragma once

#include <cstdint>

#include "../../contracts/plugin.h"

// 任务完成度进档重算（quest 插件业务层，ARCHITECTURE.md §3.3）。
//
// 背景：游戏只在 INVEN_SaveItem / 击杀 / 对象交互等事件时把任务由 state=1 置为 2（可交付），
// 没有轮询、读档也不重算。扩展背包 merge/adopt 绕过 INVEN_SaveItem → QUESTSYSTEM_OnEvent，
// 或读档后状态表逐字恢复，都会让「目标已达成」的任务永久停在 state=1。
//
// 语义：进入存档（读档/新档）加载完成、进入 world 后，注册一次性逻辑帧任务：遍历活动任务槽，
// 对 state==1 的任务调用 QUESTSYSTEM_IsComplete；游戏自判达成时走 QUESTSYSTEM_ChangeQuestState
// (qid, 2) 补齐状态。只做 1→2 补升，不做降级。
//
// 帧节拍经 Host SDK scheduler 逻辑帧点（帧源 wrapper 派发）；通过 Game Host world 状态边沿
// 识别进档完成，避免依赖宿主私有 save_enter 设施。

namespace qol::features::core_quest {

const contracts::QolHostSdk* host_sdk();
const contracts::QolGameHost* game_host();

void log_info(const char* format, ...);

// 门禁：install/on_enabled 置 true，deactivate/on_disabled 置 false。
// on_save_enter 与 tick 均检查；deactivate 后回调 no-op。
void set_gate(bool enabled);
bool gate_enabled();

// 移除当前一次性重算任务；停用和重新进档前调用，保证不会遗留旧任务。
void cancel_resync_task();

void start_resync_task();

// save_enter 回调（游戏主线程、world 就绪后触发）：注册一次性逻辑帧任务。
void on_save_enter(void* ctx);

// 一次性逻辑帧任务：补齐 state=1 且已达成者；返回 false 自动注销。
int32_t resync_tick(int64_t frame, void* ctx);

}  // namespace qol::features::core_quest
