// game_save.cpp —— 存档域：当前存档槽 + 存档槽列表（parse 域）
// 由 game_misc.cpp 拆分生成（纯搬代码，零逻辑变更）。

#include "game_save.h"

#include <cstdio>
#include <atomic>
#include <unistd.h>

#include "feature/ui/game_ui_savebackup.h"
#include "game_access.h"
#include "game_ops_common.h"
#include "game_save_preflight.h"
#include "game_state.h"
#include "core/native/module_save_port.h"
#include "core/native/extension_bag_port.h"
#include "core/native/save_enter.h"
#include "core/native/transition_dispatch.h"

// v0.5.5：当前加载存档槽（S5）——G_CURRENT_SLOT 双层解引用（SaveSlot_GoToNewGame/STATE_EnterGame 写，v0.5.5 frida 实测 world=0）
std::string data_current_save_slot_json() {
    return "{\"current_save_slot\":" + std::to_string(current_save_slot()) + "}";
}

std::string data_save_slots_json() {
    if (fn_save_get_save_slot == nullptr || fn_saveslot_get_hero == nullptr || fn_save_create_save_slot == nullptr)
        return op_err("symbol not resolved");
    // SAVE_CreateSaveSlot 会重载三槽并覆盖角色相关全局；只允许在主菜单刷新。
    // world/启动过渡阶段查询 save_slots 必须只读现有槽结构，不能因 GET /system/info 破坏当前游戏。
    if (g_state != nullptr && *reinterpret_cast<uint16_t*>(g_state) == 4)
        fn_save_create_save_slot();
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

std::string data_op_save() {
    if (!game_in_world()) return op_err("not in game");
    if (const char* ui_block = savebackup_ui_block_reason()) return op_err(ui_block);
    if (fn_save == nullptr) return op_err("symbol not resolved");
    return module_save_game() ? op_ok() : op_err("save failed");
}

// 预检（backlog P0②）：仅主菜单（STATE==4）刷新判决——非主菜单调 CreateSaveSlot 会让
// SAVE_LoadInformation 覆盖世界态全局（版本/时长等，docs/system/save.md §3），故其余状态
// 一律返回 unknown，不刷新、只读当前槽结构字节。
static std::string save_preflight_for_enter(int32_t slot) {
    if (g_state == nullptr) return op_err("libgame not ready");
    if (fn_save_get_save_slot == nullptr || fn_save_load_save_slot == nullptr ||
        fn_saveslot_get_hero == nullptr)
        return op_err("symbol not resolved");
    if (slot < 0 || slot > 2) return op_err("bad slot");
    uint16_t st = *reinterpret_cast<uint16_t*>(g_state);
    if (st != 4) {
        std::string e = "not in main menu (state=" + std::to_string(st) + ")";
        return op_err(e.c_str());
    }
    if (g_popup_on != nullptr && *reinterpret_cast<uint8_t*>(g_popup_on))
        return op_err("ui occupied: dialog_popup");
    if (const char* ui_block = savebackup_ui_block_reason()) {
        return op_err(ui_block);
    }
    void* ss = fn_save_get_save_slot(slot);
    if (ss == nullptr) return op_err("bad slot");
    fn_save_load_save_slot(slot, ss);
    uint8_t* p = reinterpret_cast<uint8_t*>(ss);
    uint8_t slot_state = p[2];
    uint8_t slot_err = p[3];
    int map_id = *reinterpret_cast<uint16_t*>(p);
    int hero_level = 0;
    int hero_index = -1;
    std::string detail;
    if (slot_state == 2) {
        void* hero = fn_saveslot_get_hero(ss);
        hero_index = *reinterpret_cast<int8_t*>(p + SAVESLOT_HERO_INDEX);
        if (hero == nullptr) {
            void* raw_hero = nullptr;
            if (hero_index >= 0 && hero_index < 3) {
                raw_hero = *reinterpret_cast<void**>(p + SAVESLOT_HERO_PTRS + hero_index * sizeof(void*));
            }
            if (raw_hero == nullptr) {
                uint8_t* player_indices = *reinterpret_cast<uint8_t**>(g_base + G_PLAYER_INDICES_GOT_VMA);
                std::string detail = "slot state is loaded but raw hero pointer is null";
                if (player_indices != nullptr && g_main_merc_slot != nullptr) {
                    detail += "; player_indices=" + std::to_string(static_cast<int>(static_cast<int8_t>(player_indices[0])))
                        + "," + std::to_string(static_cast<int>(static_cast<int8_t>(player_indices[1])))
                        + "," + std::to_string(static_cast<int>(static_cast<int8_t>(player_indices[2])))
                        + ", main_merc_slot=" + std::to_string(static_cast<int>(static_cast<int8_t>(*reinterpret_cast<uint8_t*>(g_main_merc_slot))));
                }
                return save_preflight_semantic_error_json(slot, "character", 7, detail.c_str());
            }
            return save_preflight_semantic_error_json(
                slot, "character", 7,
                "slot raw hero pointer is non-null but SAVESLOT_GetHero returned null");
        }
        hero_level = static_cast<int8_t>(*reinterpret_cast<int8_t*>(reinterpret_cast<uint8_t*>(hero) + C_LEVEL));
        uint8_t* player_indices = *reinterpret_cast<uint8_t**>(g_base + G_PLAYER_INDICES_GOT_VMA);
        if (player_indices != nullptr && g_main_merc_slot != nullptr) {
            detail = "player_indices=" + std::to_string(static_cast<int>(static_cast<int8_t>(player_indices[0])))
                + "," + std::to_string(static_cast<int>(static_cast<int8_t>(player_indices[1])))
                + "," + std::to_string(static_cast<int>(static_cast<int8_t>(player_indices[2])))
                + ", main_merc_slot=" + std::to_string(static_cast<int>(static_cast<int8_t>(*reinterpret_cast<uint8_t*>(g_main_merc_slot))));
        }
    }
    if (save_preflight_classify(slot_state, slot_err) != SavePreflightVerdict::kValid) {
        return save_preflight_error_json(slot, slot_state, slot_err);
    }
    return save_preflight_json(slot, slot_state, slot_err, map_id, hero_level, hero_index,
                               detail.empty() ? nullptr : detail.c_str());
}

static std::string enter_slot_locked(int32_t slot) {
    if (g_state == nullptr) return op_err("libgame not ready");
    if (const char* ui_block = savebackup_ui_block_reason()) return op_err(ui_block);
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
        fn_game_start_resume_game == nullptr || fn_save_create_save_slot == nullptr ||
        fn_save_load_save_slot == nullptr || fn_saveslot_get_hero == nullptr)
        return op_err("symbol not resolved");
    if (slot < 0 || slot > 2) return op_err("bad slot");
    fn_save_create_save_slot();
    std::string preflight = save_preflight_for_enter(slot);
    if (preflight.find("\"verdict\":\"valid\"") == std::string::npos) return preflight;
    void* slot_struct = fn_save_get_save_slot(slot);
    if (slot_struct == nullptr || fn_saveslot_get_hero(slot_struct) == nullptr) return op_err("slot corrupt");
    if (!extension_bag_prepare_save_slot_load()) {
        return op_err("extension bag cleanup failed; enter slot blocked");
    }
    fn_ui_set_popup_process_info(4, 0);
    uint8_t** flag_ptr = reinterpret_cast<uint8_t**>(g_base + G_GAME_RESUME_FLAG_GOT_VMA);
    if (*flag_ptr != nullptr) **flag_ptr = 0;
    save_enter_mark_pending();  // 进入存档回调：标记读档发起
    int r = fn_game_start_resume_game(slot);
    if (!r) return op_err("enter slot failed");

    // v0.4.49：进档后清理残留教学暂停——obj170=6（药水教学）是持久状态，
    // 回主菜单（GAMESTATE_SetState(4)）与 GAME_StartResumeGame 均不清理，
    // API 进档后若仍为 6 会残留 tutorial_pause 卡住移动。手动进档无此问题
    // （用户从正常世界态回主菜单时 obj170 已非 6）。进档即复位教学。
    if (tutorial_state() == 6) tutorial_cancel();
    return op_ok();
}

std::string data_op_enter_slot(int32_t slot) {
    // 读档链（GAME_StartResumeGame/SAVE 系列）必须在游戏主线程的逻辑帧点执行。
    std::string result;
    if (!transition_run([slot](std::string* out) { *out = enter_slot_locked(slot); },
                        kTransitionHeavyTimeoutMs, &result)) {
        return op_err(result.c_str());
    }
    return result;
}
// v0.4.64：创建新角色存档（复刻官方 SaveSlot_GoToNewGame + SelectCharacter_ButtonStartExe 链，
// frida 全流程监听实证，见 docs/systems/save.md §10）
static std::string create_slot_locked(int32_t slot, int32_t class_idx) {
    if (g_state == nullptr) return op_err("libgame not ready");
    if (const char* ui_block = savebackup_ui_block_reason()) return op_err(ui_block);
    uint16_t st = *reinterpret_cast<uint16_t*>(g_state);
    if (st == 5) return op_err("already in game");
    if (st != 4) {
        std::string e = "not in main menu (state=" + std::to_string(st) + ")";
        return op_err(e.c_str());
    }
    if (slot < 0 || slot > 2) return op_err("bad slot");
    if (class_idx < 0 || class_idx > 5) return op_err("bad class");
    if (g_base == 0) return op_err("libgame not ready");
    if (!extension_bag_prepare_save_slot_load()) {
        return op_err("extension bag cleanup failed; create slot blocked");
    }
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
    save_enter_mark_pending();  // 进入存档回调：标记新档发起
    fn_select_character_start_game();
    // 新档教学初始化（SelectCharacter_ButtonStartExe：StartGame 后 TutorialStart）
    fn_tutorial_start();
    // 状态机驱动：STATE_NextStartProcess → STATE_EnterGame → GAME_StartNewGame → 剧情 → 初始营地
    return op_ok();
}

std::string data_op_create_slot(int32_t slot, int32_t class_idx) {
    // 新档链（GAME_Initialize/SelectCharacter/STATE_Set）必须在游戏主线程的逻辑帧点执行。
    std::string result;
    if (!transition_run(
            [slot, class_idx](std::string* out) { *out = create_slot_locked(slot, class_idx); },
            kTransitionHeavyTimeoutMs, &result)) {
        return op_err(result.c_str());
    }
    return result;
}
