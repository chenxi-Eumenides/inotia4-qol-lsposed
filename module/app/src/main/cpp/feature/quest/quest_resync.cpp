#include "feature/quest/quest_resync.h"

#include "core/native/frame_task.h"
#include "core/native/save_enter.h"
#include "game_access.h"
#include "game_state.h"

#include <cstdint>

namespace {

// 活动任务槽上限：QUESTSYSTEM_ChangeQuestState 内部按 0x2c(44) 判满，槽数 u8 实际 ≤20。
constexpr uint8_t kMaxQuestSlots = 20;

// 一次性逻辑帧任务：补齐「目标已达成但状态停在进行中」的任务。
// 只在游戏主线程（kFramePointLogicPre 派发）执行；返回 false 自动注销。
bool quest_resync_tick(int64_t /*frame*/, void* /*ctx*/) {
    if (g_base == 0 || !game_in_world()) return false;
    if (fn_questsystem_is_complete == nullptr || fn_questsystem_change_quest_state == nullptr) return false;

    // 槽数（GOT 单层解引用 u8）、槽数组（GOT → 指针 → 数组，步长 12B，+0 u16 questId）、
    // 状态表（GOT → 二级指针 → 数组，索引 = questId）。与 api/native/game_quest.cpp 同源。
    uint8_t* cnt_ptr = *reinterpret_cast<uint8_t**>(g_base + G_QUEST_SLOT_COUNT_VMA);
    uint8_t* slots_ptr = *reinterpret_cast<uint8_t**>(g_base + G_QUEST_SLOTS_GOT_VMA);
    uint8_t*** st_got = reinterpret_cast<uint8_t***>(g_base + G_NPC_QUEST_STATE_GOT_VMA);
    if (cnt_ptr == nullptr || slots_ptr == nullptr || st_got == nullptr || *st_got == nullptr || **st_got == nullptr) {
        return false;
    }
    const uint8_t cnt = *cnt_ptr;
    uint8_t* slots = *reinterpret_cast<uint8_t**>(slots_ptr);
    uint8_t* states = **st_got;
    if (slots == nullptr || cnt == 0 || cnt > kMaxQuestSlots) return false;

    // 状态表遍历上限（quest 总数，GOT 双层解引用 u16），用于越界防护。
    uint16_t* qcnt_ptr = *reinterpret_cast<uint16_t**>(g_base + G_QUEST_COUNT_GOT_VMA);
    const uint16_t qcnt = (qcnt_ptr != nullptr) ? *qcnt_ptr : 0;

    for (uint8_t i = 0; i < cnt; ++i) {
        const uint16_t qid = *reinterpret_cast<uint16_t*>(slots + i * 0xC);
        if (qcnt != 0 && qid >= qcnt) continue;
        if (states[qid] != 1) continue;
        if (fn_questsystem_is_complete(static_cast<int32_t>(qid)) != 0) {
            fn_questsystem_change_quest_state(static_cast<int32_t>(qid), 2);
        }
    }
    return false;  // 一次性：返回 false 自动注销
}

void quest_resync_on_save_enter(void* /*ctx*/) {
    // world 已就绪；在下一个逻辑帧点执行一次（count=1 触发后自动注销）。
    frame_task_add(kFramePointLogicPre, &quest_resync_tick, nullptr, 1, 1);
}

}  // namespace

void quest_resync_register_save_enter() {
    save_enter_register(&quest_resync_on_save_enter, nullptr);
}
