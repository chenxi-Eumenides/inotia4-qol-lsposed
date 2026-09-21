#include "game/access/quest.h"

#include "game/symbols/game_symbols.h"
#include "game_access.h"

namespace qol::game {
namespace {

// 活动任务槽上限：QUESTSYSTEM_ChangeQuestState 内部按 0x2c(44) 判满，槽数 u8 实际 <=20。
constexpr uint8_t kMaxQuestSlots = 20;
// 任务槽步长：12B/槽，+0 为 u16 questId（QUESTSYSTEM_Find 0x12292c）。
constexpr uint32_t kQuestSlotStride = 0xC;

// 槽数（GOT 单层解引用 u8）；未就绪返回 nullptr。
uint8_t* slot_count_ptr() {
    if (g_base == 0) return nullptr;
    return *reinterpret_cast<uint8_t**>(g_base + G_QUEST_SLOT_COUNT_VMA);
}

// 槽数组基址（GOT -> 指针 -> 数组）；任一环为空返回 nullptr。
uint8_t* slots_array() {
    if (g_base == 0) return nullptr;
    uint8_t* slots_got = *reinterpret_cast<uint8_t**>(g_base + G_QUEST_SLOTS_GOT_VMA);
    if (slots_got == nullptr) return nullptr;
    return *reinterpret_cast<uint8_t**>(slots_got);
}

// 状态表（GOT -> 二级指针 -> 数组，索引 = questId）；任一环为空返回 nullptr。
uint8_t* states_array() {
    if (g_base == 0) return nullptr;
    uint8_t*** st_got = reinterpret_cast<uint8_t***>(g_base + G_NPC_QUEST_STATE_GOT_VMA);
    if (st_got == nullptr || *st_got == nullptr || **st_got == nullptr) return nullptr;
    return **st_got;
}

// quest 总数（GOT 双层解引用 u16）；未就绪返回 nullptr。
uint16_t* quest_count_ptr() {
    if (g_base == 0) return nullptr;
    return *reinterpret_cast<uint16_t**>(g_base + G_QUEST_COUNT_GOT_VMA);
}

}  // namespace

uint32_t quest_active_slot_count() {
    uint8_t* cnt_ptr = slot_count_ptr();
    if (cnt_ptr == nullptr) return 0;
    const uint8_t count = *cnt_ptr;
    if (count == 0 || count > kMaxQuestSlots) return 0;  // 异常值不越界
    return static_cast<uint32_t>(count);
}

bool quest_active_slot_id(uint32_t index, uint16_t* out_id) {
    if (out_id == nullptr) return false;
    const uint32_t count = quest_active_slot_count();
    if (count == 0 || index >= count) return false;
    uint8_t* slots = slots_array();
    if (slots == nullptr) return false;
    *out_id = *reinterpret_cast<const uint16_t*>(slots + index * kQuestSlotStride);
    return true;
}

uint16_t quest_total_count() {
    uint16_t* cnt_ptr = quest_count_ptr();
    return cnt_ptr != nullptr ? *cnt_ptr : 0;
}

int32_t quest_state(uint16_t quest_id) {
    uint8_t* states = states_array();
    if (states == nullptr) return -1;
    const uint16_t total = quest_total_count();
    if (total != 0 && quest_id >= total) return -1;  // 总数已知时防越界
    return static_cast<int32_t>(states[quest_id]);
}

bool quest_set_state(uint16_t quest_id, int32_t state) {
    // 走游戏自身入口而非直写状态表：保留 Add(幂等)/MAPSYSTEM_AddQuestLinkAsQuest/
    // 写 state/CHARSYSTEM_ResetInfoState 等副作用（与原 quest_resync 一致）。
    if (fn_questsystem_change_quest_state == nullptr) return false;
    fn_questsystem_change_quest_state(static_cast<int32_t>(quest_id), state);
    return true;
}

bool quest_is_complete(uint16_t quest_id) {
    if (fn_questsystem_is_complete == nullptr) return false;
    return fn_questsystem_is_complete(static_cast<int32_t>(quest_id)) != 0;
}

}  // namespace qol::game
