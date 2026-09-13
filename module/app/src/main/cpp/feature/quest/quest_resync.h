#pragma once

// 任务完成度进档重算（quest-resync）。
//
// 背景：游戏只在 INVEN_SaveItem / 击杀 / 对象交互等事件时把任务由 state=1 置为 2（可交付），
// 没有轮询、读档也不重算。扩展背包 merge/adopt 绕过 INVEN_SaveItem → QUESTSYSTEM_OnEvent，
// 或读档后状态表逐字恢复，都会让「目标已达成」的任务永久停在 state=1（NPC 问号灰、无交付提示）。
//
// 语义：进入存档（读档/新档）加载完成、进入 world 后，注册一个一次性逻辑帧任务：遍历活动
// 任务槽，对 state==1 的任务调用游戏自身的 QUESTSYSTEM_IsComplete；游戏自判达成时调用
// QUESTSYSTEM_ChangeQuestState(qid, 2) 补齐状态。
//
// 方向性：只做 1→2 补升，不做 2→1 降级。type5/6/7（脚本/剧情直驱）的 IsComplete 恒为 false，
// 但其 state 可由脚本合法置为 2；若按 IsComplete==false 降级会破坏这些任务。
//
// 无开关：进档即执行一次。IsComplete 纯读；ChangeQuestState(2) 分支不调 UI、不发奖、不存档，
// 仅 Add(幂等) + MAPSYSTEM_AddQuestLinkAsQuest + 写 state + CHARSYSTEM_ResetInfoState。

// 注册「进入存档」回调（nativeInit 调用一次，幂等）：回调在游戏主线程、world 就绪后触发，
// 注册一次性逻辑帧任务执行重算。
void quest_resync_register_save_enter();
