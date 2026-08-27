// game_save.cpp —— 存档域：当前存档槽 + 存档槽列表（parse 域）
// 由 game_misc.cpp 拆分生成（纯搬代码，零逻辑变更）。

#include "game_save.h"

#include "game_access.h"
#include "game_ops_common.h"
#include "game_state.h"
#include "game_ui_virtbag.h"

// v0.5.5：当前加载存档槽（S5）——G_CURRENT_SLOT 双层解引用（SaveSlot_GoToNewGame/STATE_EnterGame 写，v0.5.5 frida 实测 world=0）
std::string data_current_save_slot_json() {
    return "{\"current_save_slot\":" + std::to_string(current_save_slot()) + "}";
}

std::string data_save_slots_json() {
    if (fn_save_get_save_slot == nullptr || fn_saveslot_get_hero == nullptr || fn_save_create_save_slot == nullptr)
        return op_err("symbol not resolved");
    fn_save_create_save_slot();
    std::string s = "{\"slots\":[";
    for (int i = 0; i < 3; ++i) {
        if (i > 0) s += ",";
        void* slot = fn_save_get_save_slot(i);
        uint8_t b2 = slot ? *reinterpret_cast<uint8_t*>(reinterpret_cast<uint8_t*>(slot) + 2) : 0;
        int8_t hero_idx = slot ? *reinterpret_cast<int8_t*>(reinterpret_cast<uint8_t*>(slot) + 0x1c) : -1;
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

std::string data_op_save() {
    if (!game_in_world()) return op_err("not in game");
    if (fn_save == nullptr) return op_err("symbol not resolved");
    return virtual_bag_save_game() ? op_ok() : op_err("save failed");
}
std::string data_op_enter_slot(int32_t slot) {
    if (g_state == nullptr) return op_err("libgame not ready");
    uint16_t st = *reinterpret_cast<uint16_t*>(g_state);
    if (st == 5) return op_err("already in game");
    // 前置检查（v0.5.7）：仅主菜单（STATE==4）可进档。loading/切换态（STATE=0xFFFF）下
    // GAME_Initialize 未完成，GAME_StartResumeGame→ASSYSTEM_Initialize 空指针崩溃
    // （真机 tombstone 实测：ASNODE_Initialize+4 fault addr 0xe）
    if (st != 4) {
        std::string e = "not in main menu (state=" + std::to_string(st) + ")";
        return op_err(e.c_str());
    }
    if (fn_save_get_save_slot == nullptr || fn_ui_set_popup_process_info == nullptr ||
        fn_game_start_resume_game == nullptr || fn_save_create_save_slot == nullptr)
        return op_err("symbol not resolved");
    if (slot < 0 || slot > 2) return op_err("bad slot");
    // 先初始化槽区（SAVE_CreateSaveSlot 循环加载 3 槽存档到内存），否则 b0/b2 全 0 误判空槽
    fn_save_create_save_slot();
    void* slot_struct = fn_save_get_save_slot(slot);
    if (slot_struct == nullptr) return op_err("bad slot");
    uint8_t b0 = *reinterpret_cast<uint8_t*>(slot_struct);
    uint8_t b2 = *reinterpret_cast<uint8_t*>(reinterpret_cast<uint8_t*>(slot_struct) + 2);
    if (b0 == 0 && b2 == 0) return op_err("slot empty");
    virtual_bag_prepare_save_slot_load();
    fn_ui_set_popup_process_info(4, 0);
    uint8_t** flag_ptr = reinterpret_cast<uint8_t**>(g_base + G_GAME_RESUME_FLAG_GOT_VMA);
    if (*flag_ptr != nullptr) **flag_ptr = 0;
    int r = fn_game_start_resume_game(slot);
    if (!r) return op_err("enter slot failed");
    // v0.4.49：进档后清理残留教学暂停——obj170=6（药水教学）是持久状态，
    // 回主菜单（GAMESTATE_SetState(4)）与 GAME_StartResumeGame 均不清理，
    // API 进档后若仍为 6 会残留 tutorial_pause 卡住移动。手动进档无此问题
    // （用户从正常世界态回主菜单时 obj170 已非 6）。进档即复位教学。
    if (tutorial_state() == 6) tutorial_cancel();
    return op_ok();
}
// v0.4.64：创建新角色存档（复刻官方 SaveSlot_GoToNewGame + SelectCharacter_ButtonStartExe 链，
// frida 全流程监听实证，见 docs/systems/save.md §10）
std::string data_op_create_slot(int32_t slot, int32_t class_idx) {
    if (g_state == nullptr) return op_err("libgame not ready");
    uint16_t st = *reinterpret_cast<uint16_t*>(g_state);
    if (st == 5) return op_err("already in game");
    if (st != 4) {
        std::string e = "not in main menu (state=" + std::to_string(st) + ")";
        return op_err(e.c_str());
    }
    if (slot < 0 || slot > 2) return op_err("bad slot");
    if (class_idx < 0 || class_idx > 5) return op_err("bad class");
    if (g_base == 0) return op_err("libgame not ready");
    virtual_bag_prepare_save_slot_load();
    if (fn_save_create_save_slot == nullptr || fn_game_exit_save_slot_select_char == nullptr ||
        fn_select_character_start_game == nullptr || fn_tutorial_start == nullptr ||
        fn_save_get_save_file_name == nullptr || fn_cs_fs_remove == nullptr)
        return op_err("symbol not resolved");
    // 槽区初始化（SAVE_CreateSaveSlot 循环加载 3 槽存档到内存，确保槽位状态可用）
    fn_save_create_save_slot();
    // 删除目标槽旧存档文件（SaveSlot_GoToNewGame 官方链：SAVE_GetSaveFileName + CS_fsRemove）
    char fname[128] = {0};
    fn_save_get_save_file_name(slot, fname);
    if (fname[0] != '\0') fn_cs_fs_remove(fname, 1);
    // 当前槽位（SaveSlot_GoToNewGame：*[0x2f4000+0xd20] = slot）
    uint8_t** cur_slot = reinterpret_cast<uint8_t**>(g_base + G_CURRENT_SLOT_GOT_VMA);
    if (cur_slot == nullptr || *cur_slot == nullptr) return op_err("save slot state not ready");
    **cur_slot = static_cast<uint8_t>(slot);
    // 新建标志（SaveSlot_GoToNewGame：*[0x2f6000+0x8] = 1；STATE_EnterGame 检测后走 GAME_StartNewGame）
    uint8_t** newgame_flag = reinterpret_cast<uint8_t**>(g_base + G_GAME_RESUME_FLAG_GOT_VMA);
    if (newgame_flag == nullptr || *newgame_flag == nullptr) return op_err("newgame flag not ready");
    **newgame_flag = 1;
    // 选中职业（SelectCharacter_StartGame 读取源 [0x308080+0x8]）
    *reinterpret_cast<uint32_t*>(g_base + G_SELECTED_CLASS_VMA) = static_cast<uint32_t>(class_idx);
    // 进入选角环境（GAME_Initialize + MAP_Load(6) + MAINMENU_CreateSelectCharList）
    fn_game_exit_save_slot_select_char();
    // 选角确认开始（*[0x2f5000+0xa00] = class_idx + STATE_Set(5) + UI_SetPopupProcessInfo(4,0)）
    fn_select_character_start_game();
    // 新档教学初始化（SelectCharacter_ButtonStartExe：StartGame 后 TutorialStart）
    fn_tutorial_start();
    // 状态机驱动：STATE_NextStartProcess → STATE_EnterGame → GAME_StartNewGame → 剧情 → 初始营地
    return op_ok();
}
