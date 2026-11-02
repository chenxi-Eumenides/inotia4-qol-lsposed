#pragma once

#include <cstdint>

// 游戏层：任务系统语义化访问（ARCHITECTURE.md §5）。
// GOT/VMA 与游戏函数指针收口在本层；上层只调用语义函数，不出现裸地址/裸偏移。
//
// 线程纪律：以下函数只应在游戏主线程调用（依赖任务槽与状态表全局）。
// 全部要求 g_base 就绪且表指针非空；未就绪按各自返回值给「空/失败」，不解引用空指针。

namespace qol::game {

// 活动任务槽数。内部按 <=20 防越界；未就绪/异常返回 0。
uint32_t quest_active_slot_count();

// index 槽 +0 的 u16 questId；未就绪/越界/out 为空返回 false。
bool quest_active_slot_id(uint32_t index, uint16_t* out_id);

// 状态表遍历上限（quest 总数，GOT 双层解引用 u16）；未就绪返回 0。
uint16_t quest_total_count();

// 状态表读取；未就绪返回 -1。总数已知时 quest_id 越界返回 -1。
int32_t quest_state(uint16_t quest_id);

// 状态写入（沿用游戏 QUESTSYSTEM_ChangeQuestState，保留 Add/任务链/信息态等副作用）；
// 未就绪/函数指针未解析返回 false。
bool quest_set_state(uint16_t quest_id, int32_t state);

// QUESTSYSTEM_IsComplete 判定；函数指针未解析返回 false。
bool quest_is_complete(uint16_t quest_id);

// 放弃任务结果（固定宽度 POD，无 JSON/std::string）：ok=false 时 error 指向静态文案
//（"not in game"/"symbol not resolved"/"quest not found"/"quest quit failed"），调用方不释放。
struct QuestQuitResult {
    bool ok = false;
    const char* error = nullptr;
};

// 放弃任务：QUESTSYSTEM_Find 定位槽 → QUESTSYSTEM_RemoveSlot 摘除（复刻原版任务面板放弃链两步）。
// 不刷帧缓存——成功路径的 refresh 由调用方（bridge wrapper / game_system 拼装）执行恰一次。
QuestQuitResult quest_quit(int32_t quest_id);

}  // namespace qol::game
