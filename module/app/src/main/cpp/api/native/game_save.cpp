// game_save.cpp —— 存档域：当前存档槽 + 存档槽列表（parse 域）。
// save 写面已迁 game/access/save.{h,cpp}（T2l-s-write/T2l-d）；本 TU 仅保留读面（T2l-s-read 面）。

#include "game_save.h"

#include "game/access/game_access.h"
#include "game/access/save.h"
#include "game/ops/game_ops_common.h"
#include "game/state/game_state.h"

// v0.5.5：当前加载存档槽（S5）——G_CURRENT_SLOT 双层解引用（SaveSlot_GoToNewGame/STATE_EnterGame 写，v0.5.5 frida 实测 world=0）
std::string data_current_save_slot_json() {
    return "{\"current_save_slot\":" + std::to_string(current_save_slot()) + "}";
}

std::string data_save_slots_json() {
    if (fn_save_get_save_slot == nullptr || fn_saveslot_get_hero == nullptr ||
        fn_save_destroy_save_slot == nullptr || fn_save_create_save_slot == nullptr)
        return op_err("symbol not resolved");
    // SAVE_CreateSaveSlot 会重载三槽并覆盖角色相关全局；只允许在主菜单刷新，且同一次主菜单停留内
    // 只重载一次（反复重载会累积游戏侧对象，见 game/access/game_access.h 刷新策略注释）。
    // world/启动过渡阶段查询 save_slots 必须只读现有槽结构，不能因 GET /system/info 破坏当前游戏。
    qol::game::save_slots_refresh_if_stale();
    std::string s = "{\"slots\":[";
    for (int i = 0; i < 3; ++i) {
        if (i > 0) s += ",";
        void* slot = fn_save_get_save_slot(i);
        uint8_t b2 = slot ? *reinterpret_cast<uint8_t*>(reinterpret_cast<uint8_t*>(slot) + SAVESLOT_EXISTS) : 0;
        int8_t hero_idx = slot ? *reinterpret_cast<int8_t*>(reinterpret_cast<uint8_t*>(slot) + SAVESLOT_HERO_INDEX) : -1;
        bool exists = (b2 != 0);
        s += "{\"slot\":" + std::to_string(i) + ",\"exists\":" + (exists ? "true" : "false");
        if (exists) {
            void* hero = fn_saveslot_get_hero(slot);
            int level = hero ? static_cast<int8_t>(*reinterpret_cast<int8_t*>(reinterpret_cast<uint8_t*>(hero) + C_LEVEL)) : 0;
            s += ",\"hero_level\":" + std::to_string(level) + ",\"hero_index\":" + std::to_string(hero_idx);
        }
        s += "}";
    }
    s += "]}";
    return s;
}
