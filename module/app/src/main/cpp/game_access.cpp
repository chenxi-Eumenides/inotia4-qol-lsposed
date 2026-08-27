#include "game_access.h"
#include "game_patch.h"
#include "symbol_resolver.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

uintptr_t g_base = 0;
void* g_money = nullptr;
void* g_map_id = nullptr;
void* g_party = nullptr;
void* g_active_quest = nullptr;
void* g_inven = nullptr;
void* g_main_merc_slot = nullptr;
void* g_prev_state = nullptr;
void* g_state = nullptr;
void* g_gamestate = nullptr;
void* g_initstate = nullptr;
void* g_popup_on = nullptr;
void* g_mainmenu_draw = nullptr;
void* g_popup_stack = nullptr;
void* g_player_active = nullptr;
void* g_uimix = nullptr;

GetMoneyFn fn_get_money = nullptr;
GetMemberFn fn_get_member = nullptr;
GetPartySizeFn fn_get_party_size = nullptr;
GetAttrFn fn_get_attr = nullptr;
GetEquipFn fn_get_equip = nullptr;
GetExpFn fn_get_exp = nullptr;
GetExpFn fn_get_next_exp = nullptr;
GetRarityFn fn_get_rarity = nullptr;
GetBagSizeFn fn_get_bag_size = nullptr;
GetBitFn fn_get_bit = nullptr;
GetCumulateCountFn fn_get_cumulate_count = nullptr;GetItemStatFn fn_get_damage = nullptr;
GetItemStatFn fn_get_defense = nullptr;
GetAttrFn2 fn_get_stat = nullptr;
GetAttrFn2 fn_get_stat_base = nullptr;
GetAttrFn2 fn_get_stat_bonus = nullptr;
GetStatusPointFn fn_get_status_point = nullptr;
GetStatMainFn fn_get_stat_main = nullptr;
SetStatMainFn fn_set_stat_main = nullptr;
SetStatBaseFn fn_set_stat_base = nullptr;
RollStatusDiceFn fn_status_dice_roll = nullptr;
PutJewelFn fn_put_jewel = nullptr;
IsJewelFn fn_is_jewel = nullptr;
EnchantItemFn fn_enchant_item = nullptr;
IsEnchantScrollFn fn_is_enchant_scroll = nullptr;
CharInitializeStatusFn fn_char_initialize_status = nullptr;
CharInitializeSkillFn fn_char_initialize_skill = nullptr;
CharSetActionIdFn fn_char_set_action_id = nullptr;
CharGetEnemyTargetFn fn_char_get_enemy_target = nullptr;
QuestSystemFindFn fn_questsystem_find = nullptr;
QuestSystemRemoveSlotFn fn_questsystem_remove_slot = nullptr;
SaveFn fn_save = nullptr;
SaveGetSaveSlotFn fn_save_get_save_slot = nullptr;
UiSetPopupProcessInfoFn fn_ui_set_popup_process_info = nullptr;
GameStartResumeGameFn fn_game_start_resume_game = nullptr;
SaveCreateSaveSlotFn fn_save_create_save_slot = nullptr;
SaveslotGetHeroFn fn_saveslot_get_hero = nullptr;
StateSetFn fn_state_set = nullptr;
GameExitSaveSlotSelectCharFn fn_game_exit_save_slot_select_char = nullptr;
SelectCharacterStartGameFn fn_select_character_start_game = nullptr;
TutorialStartFn fn_tutorial_start = nullptr;
SaveGetSaveFileNameFn fn_save_get_save_file_name = nullptr;
CsFsRemoveFn fn_cs_fs_remove = nullptr;
GamestateSetStateFn fn_gamestate_set_state = nullptr;
UinpcInitFn fn_uinpc_init = nullptr;
NpcSystemCheckFunctionDisplayFn fn_check_function_display = nullptr;
UinpcExeTaskFn fn_uinpc_exe_current_task = nullptr;
UinpcQuestButtonOkExeFn fn_uinpc_quest_button_ok_exe = nullptr;
NpctasklistMakeDlgFn fn_npctasklist_make_dlg = nullptr;
PlayerCheckNearNpcFn fn_player_check_near_npc = nullptr;
GetSkillUsageFn fn_get_skill_usage = nullptr;
SetSkillUsageFn fn_set_skill_usage = nullptr;
GetNameFn fn_get_name = nullptr;
GetActMaxLevelFn fn_get_act_max_level = nullptr;
FindMercSlotFn fn_find_merc_slot = nullptr;
SearchPathFn fn_search_path = nullptr;
EvtSetStateFn fn_evt_set_state = nullptr;
TextctrlMoveNextPageFn fn_textctrl_move_next_page = nullptr;
KeySetCodeFn fn_key_set_code = nullptr;
IntVoidFn fn_wipeout_button_revive = nullptr;
IntVoidFn fn_wipeout_button_special_revive = nullptr;
IntVoidFn fn_wipeout_button_gameover = nullptr;
IntVoidFn fn_event_button_ok_exe = nullptr;
IntVoidFn fn_event_button_skip_exe = nullptr;
IntIntFn fn_evtsystem_do_check_all_event = nullptr;
IntVoidFn fn_tutorial_getstate = nullptr;

SetMoneyFn fn_set_money = nullptr;
AddMoneyFn fn_add_money = nullptr;
AddMoneyFn fn_minus_money = nullptr;
RemoveItemFn fn_remove_item = nullptr;
ItemGetPriceFn fn_item_get_price = nullptr;
ItemGetAbilityLevelFn fn_item_get_ability_level = nullptr;
ItemGetBuyPriceFn fn_item_get_buy_price = nullptr;
InvenFindSaveSlotFn fn_inven_find_save_slot = nullptr;
InvenSaveItemFn fn_inven_save_item = nullptr;
InvenSaveItemDirectFn fn_inven_save_item_direct = nullptr;
InvenSaveItemOnEmptyFn fn_inven_save_item_on_empty = nullptr;
DealSystemFindSaleByIdFn fn_dealsystem_find_sale_by_id = nullptr;
InvenMoveItemFn fn_inven_move_item = nullptr;
SetExpFn fn_set_exp = nullptr;
SetLevelFn fn_set_level = nullptr;
AddExpFn fn_add_exp = nullptr;
SetStatusPointFn fn_set_status_point = nullptr;
SetAutoAttackFn fn_set_auto_attack = nullptr;
EquipItemFn fn_equip_item = nullptr;
UnequipFn fn_unequip = nullptr;
CanEquipFn fn_can_equip = nullptr;
FindEquipSlotFn fn_find_equip_slot = nullptr;
GetEquipItemFn fn_get_equip_item = nullptr;
IsSpecialNpcFn fn_is_special_npc = nullptr;
LearnActionFn fn_learn_action = nullptr;
SetActivePlayerFn fn_set_active_player = nullptr;
PartySwapFn fn_party_swap = nullptr;
SetPositionFn fn_set_position = nullptr;
ChangeMapFn fn_change_map = nullptr;

MoveAsPathFn fn_move_as_path = nullptr;
CharMoveFn fn_char_move = nullptr;
CharPickItemAllFn fn_char_pick_item_all = nullptr;
CharGetAreaRectFn fn_char_get_area_rect = nullptr;
MemMallocFn fn_mem_malloc = nullptr;
NotifierAddFn fn_notifier_add = nullptr;
CharSetDirectionFn fn_char_set_direction = nullptr;
CharRemovePathFn fn_char_remove_path = nullptr;
MapSetFocusFn fn_map_set_focus = nullptr;
GoMapLinkByCharFn fn_go_map_link_by_char = nullptr;
CharSetTargetFn fn_char_set_target = nullptr;
CharStopCombatFn fn_char_stop_combat = nullptr;
ConsumeItemFn fn_consume_item = nullptr;
CharUseItemExFn fn_char_use_item_ex = nullptr;
RemoveItemDirectFn fn_remove_item_direct = nullptr;
IncludePartyFn fn_include_party = nullptr;
ExcludePartyFn fn_exclude_party = nullptr;
MercenaryReleaseFn fn_mercenary_release = nullptr;
ItemIsUseFn fn_is_use = nullptr;
PopupStateExistFn fn_popup_exist = nullptr;
NetworkStoreSetStateFn fn_networkstore_set_state = nullptr;
OpenItemBoxFn fn_open_item_box = nullptr;
ReleaseSealedFn fn_release_sealed = nullptr;
IsDiceFn fn_is_dice = nullptr;
IsSealedFn fn_is_sealed = nullptr;
IsItemBoxFn fn_is_item_box = nullptr;
MakeItemFn fn_make_item = nullptr;
CreateItemFn fn_create_item = nullptr;
ControlObjectGetDataFn fn_control_object_get_data = nullptr;
UiEquipIsApplyStuffFn fn_ui_equip_is_apply_stuff = nullptr;
UiEquipGetItemSlotIndexFn fn_ui_equip_get_item_slot_index = nullptr;
UiEquipRefreshItemAreaFn fn_ui_equip_refresh_item_area = nullptr;
UiDescSetOffFn fn_ui_desc_set_off = nullptr;
TouchHandleSetCursorFn fn_touch_handle_set_cursor = nullptr;
UiEquipInvenItemControlEventProcFn fn_ui_equip_inven_item_control_event_proc = nullptr;
ItemDrawPortingFn fn_item_draw_porting = nullptr;
MakeMixFn fn_make_mix = nullptr;
GetCostFn fn_get_cost = nullptr;
SaveSaveItemFn fn_save_save_item = nullptr;
SaveLoadItemFn fn_save_load_item = nullptr;
ItemPoolFreeFn fn_itempool_free = nullptr;
// ---- UI 实验函数指针（ui-exp v0.6.7）----
ControlObjectCreateFn fn_ctrl_create = nullptr;
ControlObjectAddFn fn_ctrl_add = nullptr;
ControlObjectSetRectFn fn_ctrl_set_rect = nullptr;
ControlObjectSetEventCallTypeFn fn_ctrl_set_event_call_type = nullptr;
ControlObjectSetDataFn fn_ctrl_set_data = nullptr;
ControlButtonCreateFn fn_ctrl_btn_create = nullptr;
ControlButtonSetTextFn fn_ctrl_btn_set_text = nullptr;
ControlButtonSetDrawTypeFn fn_ctrl_btn_set_draw_type = nullptr;
ControlButtonSetDrawIDFn fn_ctrl_btn_set_draw_id = nullptr;
ControlButtonSetDrawSubIDFn fn_ctrl_btn_set_draw_sub_id = nullptr;
ControlButtonSetDrawProcFn fn_ctrl_btn_set_draw_proc = nullptr;
UiCreateGroupBaseControlFn fn_ui_create_group_base_control = nullptr;
UiPopupMsgCreateFn fn_popup_create = nullptr;
UiPopupMsgCreateYesNoFn fn_popup_create_yesno = nullptr;
UiPopupMsgCreateFromTextDataFn fn_popup_create_from_textdata = nullptr;
UiPopupMsgFreeFn fn_popup_free = nullptr;
PopupStatePushFn fn_popupstate_push = nullptr;
// ---- UI 绘制原语函数指针（ui-settings v0.6.9）----
GrpxStartFn fn_grpx_start = nullptr;
GrpxEndFn fn_grpx_end = nullptr;
GrpxFillRectFn fn_grpx_fill_rect = nullptr;
GrpxFillRectAlphaFn fn_grpx_fill_rect_alpha = nullptr;
GrpxSetFontColorFn fn_grpx_set_font_color = nullptr;
GrpxDrawStringWithFontFn fn_grpx_draw_string_with_font = nullptr;
GrpxSetFontColorRgbFn fn_grpx_set_font_color_rgb = nullptr;
GrpxDrawPartFn fn_grpx_draw_part = nullptr;
UiDrawStringHAlignFn fn_ui_draw_string_halign = nullptr;
UiDrawStringInWidthWithFontFn fn_ui_draw_string_in_width_with_font = nullptr;
MwGraphicDrawStringFn fn_mw_graphic_draw_string = nullptr;
GrpSaveLcdFn fn_grp_save_lcd = nullptr;
GrpRestoreLcdFn fn_grp_restore_lcd = nullptr;
UiSetRefreshLcdFlagFn fn_ui_set_refresh_lcd_flag = nullptr;
UiGetRefreshLcdFlagFn fn_ui_get_refresh_lcd_flag = nullptr;
CalcResolutionFn fn_calc_res_width = nullptr;
CalcResolutionFn fn_calc_res_height = nullptr;
ImgsysUnitFn fn_imgsys_unit_load = nullptr;
ImgsysUnitFn fn_imgsys_unit_unload = nullptr;
ImgsysGetGroupFn fn_imgsys_get_group = nullptr;
ImgsysGetLocFn fn_imgsys_get_loc = nullptr;
GetGroupTitleImgTypeFn fn_get_group_title_img_type = nullptr;
ControlObjectGetCountFn fn_ctrl_get_count = nullptr;
ControlObjectGetChildFn fn_ctrl_get_child = nullptr;
ControlObjectGetDataFn fn_ctrl_get_data = nullptr;
ControlObjectSetActiveFn fn_ctrl_set_active = nullptr;
ControlButtonDrawFn fn_ctrl_btn_draw = nullptr;

std::vector<std::pair<const char*, bool>> g_symbol_report;
std::string g_dl_error;
std::string g_lib_path;

namespace {

std::mutex g_mutex;

// 宏名 → 游戏符号名查找表（X-macro 注册表生成，backlog P1 VMA 治理）
const char* symbol_name_for_macro(const char* macro) {
    if (macro == nullptr || macro[0] == '\0') return nullptr;
#define SYM(macro_name, symbol) { #macro_name, #symbol },
    static const struct { const char* macro; const char* symbol; } kSymbolTable[] = {
#include "symbol_registry.h"
    };
#undef SYM
    for (const auto& e : kSymbolTable) {
        if (strcmp(e.macro, macro) == 0) return e.symbol;
    }
    return nullptr;
}

bool find_libgame_base() {
    FILE* f = fopen("/proc/self/maps", "r");
    if (f == nullptr) return false;
    char line[512];
    bool found = false;
    while (fgets(line, sizeof(line), f) != nullptr) {
        if (strstr(line, "libgame.so") != nullptr) {
            g_base = strtoul(line, nullptr, 16);
            found = true;
            char* p = strchr(line, '/');
            if (p != nullptr) {
                std::string s(p);
                s.erase(s.find_last_not_of("\n\r") + 1);
                g_lib_path = s;
            }
            break;
        }
    }
    fclose(f);
    return found;
}

// 全局变量/数据地址：有符号名 → ELF 动态解析（未命中回退 VMA），无符号名 → 直接 VMA
void resolve_global(void*& dst, uintptr_t vma, const char* macro_name) {
    const char* symbol = symbol_name_for_macro(macro_name);
    ResolvedSymbol r = g_resolver.resolve(symbol, vma);
    dst = reinterpret_cast<void*>(g_base + r.offset);
    g_symbol_report.emplace_back(macro_name, r.source != SymbolSource::MISS);
}

}  // namespace

// 函数指针地址：同 resolve_global（返回相对偏移，调用方拼 g_base）
uintptr_t fn_resolve(const char* macro_name, uintptr_t vma) {
    const char* symbol = symbol_name_for_macro(macro_name);
    ResolvedSymbol r = g_resolver.resolve(symbol, vma);
    if (r.source == SymbolSource::ELF) {
        g_symbol_report.emplace_back(macro_name, true);
    } else if (r.source == SymbolSource::MISS) {
        g_symbol_report.emplace_back(macro_name, false);
    }
    return r.offset;
}

bool bridge_init() {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_base != 0) return true;
    g_symbol_report.clear();
    if (!find_libgame_base()) {
        g_symbol_report.emplace_back("libgame_not_loaded", false);
        g_dl_error = "libgame.so not loaded yet";
        return false;
    }
    g_dl_error.clear();
    g_resolver.attach(g_base);  // 解析 .dynsym 哈希表（幂等），后续 fn_resolve/resolve_global 动态解析符号
    resolve_global(g_money, G_MONEY_VMA, "G_MONEY_VMA");
    resolve_global(g_map_id, G_MAP_ID_VMA, "G_MAP_ID_VMA");
    resolve_global(g_party, G_PARTY_VMA, "G_PARTY_VMA");
    resolve_global(g_active_quest, G_ACTIVE_QUEST_VMA, "G_ACTIVE_QUEST_VMA");
    resolve_global(g_inven, G_INVEN_VMA, "G_INVEN_VMA");
    resolve_global(g_main_merc_slot, G_MAIN_MERC_SLOT_VMA, "G_MAIN_MERC_SLOT_VMA");
    resolve_global(g_prev_state, G_PREV_STATE_VMA, "G_PREV_STATE_VMA");
    resolve_global(g_state, G_STATE_VMA, "G_STATE_VMA");
    resolve_global(g_gamestate, G_GAMESTATE_VMA, "G_GAMESTATE_VMA");
    resolve_global(g_initstate, G_INITSTATE_VMA, "G_INITSTATE_VMA");
    resolve_global(g_popup_on, G_POPUP_ON_VMA, "G_POPUP_ON_VMA");
    resolve_global(g_mainmenu_draw, G_MAINMENU_DRAW_VMA, "G_MAINMENU_DRAW_VMA");
    resolve_global(g_popup_stack, G_POPUP_STACK_VMA, "G_POPUP_STACK_VMA");
    resolve_global(g_player_active, G_PLAYER_ACTIVE_VMA, "G_PLAYER_ACTIVE_VMA");
    resolve_global(g_uimix, G_UIMIX_VMA, "G_UIMIX_VMA");
    fn_get_money = reinterpret_cast<GetMoneyFn>(g_base + fn_resolve("F_GET_MONEY_VMA", F_GET_MONEY_VMA));
    fn_get_member = reinterpret_cast<GetMemberFn>(g_base + fn_resolve("F_GET_MEMBER_VMA", F_GET_MEMBER_VMA));
    fn_get_party_size = reinterpret_cast<GetPartySizeFn>(g_base + fn_resolve("F_GET_PARTY_SIZE_VMA", F_GET_PARTY_SIZE_VMA));
    fn_get_attr = reinterpret_cast<GetAttrFn>(g_base + fn_resolve("F_GET_ATTR_VMA", F_GET_ATTR_VMA));
    fn_get_equip = reinterpret_cast<GetEquipFn>(g_base + fn_resolve("F_GET_EQUIP_VMA", F_GET_EQUIP_VMA));
    fn_get_exp = reinterpret_cast<GetExpFn>(g_base + fn_resolve("F_GET_EXP_VMA", F_GET_EXP_VMA));
    fn_get_next_exp = reinterpret_cast<GetExpFn>(g_base + fn_resolve("F_GET_NEXT_EXP_VMA", F_GET_NEXT_EXP_VMA));
    fn_get_rarity = reinterpret_cast<GetRarityFn>(g_base + fn_resolve("F_GET_RARITY_VMA", F_GET_RARITY_VMA));
    fn_get_bag_size = reinterpret_cast<GetBagSizeFn>(g_base + fn_resolve("F_GET_BAG_SIZE_VMA", F_GET_BAG_SIZE_VMA));
    fn_get_bit = reinterpret_cast<GetBitFn>(g_base + fn_resolve("F_GET_BIT_VMA", F_GET_BIT_VMA));
    fn_get_cumulate_count = reinterpret_cast<GetCumulateCountFn>(g_base + fn_resolve("F_GET_CUMULATE_COUNT_VMA", F_GET_CUMULATE_COUNT_VMA));
    fn_get_damage = reinterpret_cast<GetItemStatFn>(g_base + fn_resolve("F_GET_DAMAGE_VMA", F_GET_DAMAGE_VMA));
    fn_get_defense = reinterpret_cast<GetItemStatFn>(g_base + fn_resolve("F_GET_DEFENSE_VMA", F_GET_DEFENSE_VMA));
    fn_get_stat = reinterpret_cast<GetAttrFn2>(g_base + F_GET_STAT_VMA);
    fn_get_stat_base = reinterpret_cast<GetAttrFn2>(g_base + F_GET_STAT_BASE_VMA);
    fn_get_stat_bonus = reinterpret_cast<GetAttrFn2>(g_base + F_GET_STAT_BONUS_VMA);
    fn_get_status_point = reinterpret_cast<GetStatusPointFn>(g_base + fn_resolve("F_GET_STATUS_POINT_VMA", F_GET_STATUS_POINT_VMA));
    fn_get_stat_main = reinterpret_cast<GetStatMainFn>(g_base + fn_resolve("F_GET_STAT_MAIN_VMA", F_GET_STAT_MAIN_VMA));
    fn_set_stat_main = reinterpret_cast<SetStatMainFn>(g_base + fn_resolve("F_SET_STAT_MAIN_VMA", F_SET_STAT_MAIN_VMA));
    fn_set_stat_base = reinterpret_cast<SetStatBaseFn>(g_base + fn_resolve("F_SET_STAT_BASE_VMA", F_SET_STAT_BASE_VMA));
    fn_status_dice_roll = reinterpret_cast<RollStatusDiceFn>(g_base + fn_resolve("F_STATUSDICE_ROLL_VMA", F_STATUSDICE_ROLL_VMA));
    fn_put_jewel = reinterpret_cast<PutJewelFn>(g_base + fn_resolve("F_PUT_JEWEL_VMA", F_PUT_JEWEL_VMA));
    fn_is_jewel = reinterpret_cast<IsJewelFn>(g_base + fn_resolve("F_IS_JEWEL_VMA", F_IS_JEWEL_VMA));
    fn_enchant_item = reinterpret_cast<EnchantItemFn>(g_base + fn_resolve("F_ENCHANT_ITEM_VMA", F_ENCHANT_ITEM_VMA));
    fn_is_enchant_scroll = reinterpret_cast<IsEnchantScrollFn>(g_base + fn_resolve("F_IS_ENCHANT_SCROLL_VMA", F_IS_ENCHANT_SCROLL_VMA));
    fn_char_initialize_status = reinterpret_cast<CharInitializeStatusFn>(g_base + fn_resolve("F_CHAR_INITIALIZE_STATUS_VMA", F_CHAR_INITIALIZE_STATUS_VMA));
    fn_char_initialize_skill = reinterpret_cast<CharInitializeSkillFn>(g_base + fn_resolve("F_CHAR_INITIALIZE_SKILL_VMA", F_CHAR_INITIALIZE_SKILL_VMA));
    fn_char_set_action_id = reinterpret_cast<CharSetActionIdFn>(g_base + fn_resolve("F_CHAR_SET_ACTION_ID_VMA", F_CHAR_SET_ACTION_ID_VMA));
    fn_char_get_enemy_target = reinterpret_cast<CharGetEnemyTargetFn>(g_base + fn_resolve("F_CHAR_GET_ENEMY_TARGET_VMA", F_CHAR_GET_ENEMY_TARGET_VMA));
    fn_questsystem_find = reinterpret_cast<QuestSystemFindFn>(g_base + fn_resolve("F_QUESTSYSTEM_FIND_VMA", F_QUESTSYSTEM_FIND_VMA));
    fn_questsystem_remove_slot = reinterpret_cast<QuestSystemRemoveSlotFn>(g_base + fn_resolve("F_QUESTSYSTEM_REMOVE_SLOT_VMA", F_QUESTSYSTEM_REMOVE_SLOT_VMA));
    fn_save = reinterpret_cast<SaveFn>(g_base + fn_resolve("F_SAVE_VMA", F_SAVE_VMA));
    fn_save_get_save_slot = reinterpret_cast<SaveGetSaveSlotFn>(g_base + fn_resolve("F_SAVE_GET_SAVE_SLOT_VMA", F_SAVE_GET_SAVE_SLOT_VMA));
    fn_ui_set_popup_process_info = reinterpret_cast<UiSetPopupProcessInfoFn>(g_base + fn_resolve("F_UI_SET_POPUP_PROCESS_INFO_VMA", F_UI_SET_POPUP_PROCESS_INFO_VMA));
    fn_game_start_resume_game = reinterpret_cast<GameStartResumeGameFn>(g_base + fn_resolve("F_GAME_START_RESUME_GAME_VMA", F_GAME_START_RESUME_GAME_VMA));
    fn_save_create_save_slot = reinterpret_cast<SaveCreateSaveSlotFn>(g_base + fn_resolve("F_SAVE_CREATE_SAVE_SLOT_VMA", F_SAVE_CREATE_SAVE_SLOT_VMA));
    fn_saveslot_get_hero = reinterpret_cast<SaveslotGetHeroFn>(g_base + fn_resolve("F_SAVESLOT_GET_HERO_VMA", F_SAVESLOT_GET_HERO_VMA));
    fn_state_set = reinterpret_cast<StateSetFn>(g_base + fn_resolve("F_STATE_SET_VMA", F_STATE_SET_VMA));
    fn_game_exit_save_slot_select_char = reinterpret_cast<GameExitSaveSlotSelectCharFn>(g_base + fn_resolve("F_GAME_EXIT_SAVE_SLOT_SELECT_CHAR_VMA", F_GAME_EXIT_SAVE_SLOT_SELECT_CHAR_VMA));
    fn_select_character_start_game = reinterpret_cast<SelectCharacterStartGameFn>(g_base + fn_resolve("F_SELECT_CHARACTER_START_GAME_VMA", F_SELECT_CHARACTER_START_GAME_VMA));
    fn_tutorial_start = reinterpret_cast<TutorialStartFn>(g_base + fn_resolve("F_TUTORIAL_START_VMA", F_TUTORIAL_START_VMA));
    fn_save_get_save_file_name = reinterpret_cast<SaveGetSaveFileNameFn>(g_base + fn_resolve("F_SAVE_GET_SAVE_FILE_NAME_VMA", F_SAVE_GET_SAVE_FILE_NAME_VMA));
    fn_cs_fs_remove = reinterpret_cast<CsFsRemoveFn>(g_base + fn_resolve("F_CS_FS_REMOVE_VMA", F_CS_FS_REMOVE_VMA));
    fn_gamestate_set_state = reinterpret_cast<GamestateSetStateFn>(g_base + fn_resolve("F_GAMESTATE_SET_STATE_VMA", F_GAMESTATE_SET_STATE_VMA));
    fn_uinpc_init = reinterpret_cast<UinpcInitFn>(g_base + fn_resolve("F_UINPC_INIT_VMA", F_UINPC_INIT_VMA));
    fn_check_function_display = reinterpret_cast<NpcSystemCheckFunctionDisplayFn>(g_base + fn_resolve("F_NPCSYSTEM_CHECK_FUNCTION_DISPLAY_VMA", F_NPCSYSTEM_CHECK_FUNCTION_DISPLAY_VMA));
    fn_uinpc_exe_current_task = reinterpret_cast<UinpcExeTaskFn>(g_base + fn_resolve("F_UINPC_EXE_CURRENT_TASK_VMA", F_UINPC_EXE_CURRENT_TASK_VMA));
    fn_uinpc_quest_button_ok_exe = reinterpret_cast<UinpcQuestButtonOkExeFn>(g_base + fn_resolve("F_UINPC_QUEST_BUTTON_OK_EXE_VMA", F_UINPC_QUEST_BUTTON_OK_EXE_VMA));
    fn_npctasklist_make_dlg = reinterpret_cast<NpctasklistMakeDlgFn>(g_base + fn_resolve("F_NPCTASKLIST_MAKE_DLG_VMA", F_NPCTASKLIST_MAKE_DLG_VMA));
    fn_player_check_near_npc = reinterpret_cast<PlayerCheckNearNpcFn>(g_base + fn_resolve("F_PLAYER_DO_CHECK_NEAR_NPC_VMA", F_PLAYER_DO_CHECK_NEAR_NPC_VMA));
    fn_get_skill_usage = reinterpret_cast<GetSkillUsageFn>(g_base + fn_resolve("F_CHAR_GET_SKILL_USAGE_VMA", F_CHAR_GET_SKILL_USAGE_VMA));
    fn_set_skill_usage = reinterpret_cast<SetSkillUsageFn>(g_base + fn_resolve("F_CHAR_SET_SKILL_USAGE_VMA", F_CHAR_SET_SKILL_USAGE_VMA));
    fn_get_name = reinterpret_cast<GetNameFn>(g_base + fn_resolve("F_GET_NAME_VMA", F_GET_NAME_VMA));
    fn_get_act_max_level = reinterpret_cast<GetActMaxLevelFn>(g_base + fn_resolve("F_GET_ACT_MAX_LEVEL_VMA", F_GET_ACT_MAX_LEVEL_VMA));
    fn_find_merc_slot = reinterpret_cast<FindMercSlotFn>(g_base + fn_resolve("F_FIND_MERC_SLOT_VMA", F_FIND_MERC_SLOT_VMA));
    fn_search_path = reinterpret_cast<SearchPathFn>(g_base + fn_resolve("F_SEARCH_PATH_VMA", F_SEARCH_PATH_VMA));
    fn_evt_set_state = reinterpret_cast<EvtSetStateFn>(g_base + fn_resolve("F_EVT_SET_STATE_VMA", F_EVT_SET_STATE_VMA));
    fn_textctrl_move_next_page = reinterpret_cast<TextctrlMoveNextPageFn>(g_base + fn_resolve("F_TEXTCTRL2_MOVE_NEXT_PAGE_VMA", F_TEXTCTRL2_MOVE_NEXT_PAGE_VMA));
    fn_key_set_code = reinterpret_cast<KeySetCodeFn>(g_base + fn_resolve("F_KEY_SET_CODE_VMA", F_KEY_SET_CODE_VMA));
    fn_wipeout_button_revive = reinterpret_cast<IntVoidFn>(g_base + fn_resolve("F_WIPEOUT_BUTTON_REVIVE_VMA", F_WIPEOUT_BUTTON_REVIVE_VMA));
    fn_wipeout_button_special_revive = reinterpret_cast<IntVoidFn>(g_base + fn_resolve("F_WIPEOUT_BUTTON_SPECIAL_REVIVE_VMA", F_WIPEOUT_BUTTON_SPECIAL_REVIVE_VMA));
    fn_wipeout_button_gameover = reinterpret_cast<IntVoidFn>(g_base + fn_resolve("F_WIPEOUT_BUTTON_GAMEOVER_VMA", F_WIPEOUT_BUTTON_GAMEOVER_VMA));
    fn_event_button_ok_exe = reinterpret_cast<IntVoidFn>(g_base + fn_resolve("F_EVENT_BUTTON_OK_EXE_VMA", F_EVENT_BUTTON_OK_EXE_VMA));
    fn_event_button_skip_exe = reinterpret_cast<IntVoidFn>(g_base + fn_resolve("F_EVENT_BUTTON_SKIP_EXE_VMA", F_EVENT_BUTTON_SKIP_EXE_VMA));
    fn_evtsystem_do_check_all_event = reinterpret_cast<IntIntFn>(g_base + fn_resolve("F_EVTSYSTEM_DO_CHECK_ALL_EVENT_VMA", F_EVTSYSTEM_DO_CHECK_ALL_EVENT_VMA));
    fn_tutorial_getstate = reinterpret_cast<IntVoidFn>(g_base + fn_resolve("F_TUTORIAL_GETSTATE_VMA", F_TUTORIAL_GETSTATE_VMA));
    fn_set_money = reinterpret_cast<SetMoneyFn>(g_base + fn_resolve("F_SET_MONEY_VMA", F_SET_MONEY_VMA));
    fn_add_money = reinterpret_cast<AddMoneyFn>(g_base + fn_resolve("F_ADD_MONEY_VMA", F_ADD_MONEY_VMA));
    fn_minus_money = reinterpret_cast<AddMoneyFn>(g_base + fn_resolve("F_MINUS_MONEY_VMA", F_MINUS_MONEY_VMA));
    fn_remove_item = reinterpret_cast<RemoveItemFn>(g_base + fn_resolve("F_REMOVE_ITEM_VMA", F_REMOVE_ITEM_VMA));
    fn_item_get_price = reinterpret_cast<ItemGetPriceFn>(g_base + fn_resolve("F_ITEM_GET_PRICE_VMA", F_ITEM_GET_PRICE_VMA));
    fn_item_get_ability_level = reinterpret_cast<ItemGetAbilityLevelFn>(g_base + fn_resolve("F_ITEM_GET_ABILITY_LEVEL_VMA", F_ITEM_GET_ABILITY_LEVEL_VMA));
    fn_item_get_buy_price = reinterpret_cast<ItemGetBuyPriceFn>(g_base + fn_resolve("F_ITEM_GET_BUY_PRICE_VMA", F_ITEM_GET_BUY_PRICE_VMA));
    fn_inven_find_save_slot = reinterpret_cast<InvenFindSaveSlotFn>(g_base + fn_resolve("F_INVEN_FIND_SAVE_SLOT_VMA", F_INVEN_FIND_SAVE_SLOT_VMA));
    fn_inven_save_item = reinterpret_cast<InvenSaveItemFn>(g_base + fn_resolve("F_INVEN_SAVE_ITEM_VMA", F_INVEN_SAVE_ITEM_VMA));
    fn_inven_save_item_direct = reinterpret_cast<InvenSaveItemDirectFn>(g_base + fn_resolve("F_INVEN_SAVE_ITEM_DIRECT_VMA", F_INVEN_SAVE_ITEM_DIRECT_VMA));
    fn_inven_save_item_on_empty = reinterpret_cast<InvenSaveItemOnEmptyFn>(g_base + fn_resolve("F_INVEN_SAVE_ITEM_ON_EMPTY_VMA", F_INVEN_SAVE_ITEM_ON_EMPTY_VMA));
    fn_dealsystem_find_sale_by_id = reinterpret_cast<DealSystemFindSaleByIdFn>(g_base + fn_resolve("F_DEALSYSTEM_FIND_SALE_BY_ID_VMA", F_DEALSYSTEM_FIND_SALE_BY_ID_VMA));
    fn_inven_move_item = reinterpret_cast<InvenMoveItemFn>(g_base + fn_resolve("F_INVEN_MOVE_ITEM_VMA", F_INVEN_MOVE_ITEM_VMA));
    fn_set_exp = reinterpret_cast<SetExpFn>(g_base + fn_resolve("F_SET_EXP_VMA", F_SET_EXP_VMA));
    fn_set_level = reinterpret_cast<SetLevelFn>(g_base + fn_resolve("F_SET_LEVEL_VMA", F_SET_LEVEL_VMA));
    fn_add_exp = reinterpret_cast<AddExpFn>(g_base + fn_resolve("F_ADD_EXP_VMA", F_ADD_EXP_VMA));
    fn_set_status_point = reinterpret_cast<SetStatusPointFn>(g_base + fn_resolve("F_SET_STATUS_POINT_VMA", F_SET_STATUS_POINT_VMA));
    fn_set_auto_attack = reinterpret_cast<SetAutoAttackFn>(g_base + fn_resolve("F_SET_AUTO_ATTACK_VMA", F_SET_AUTO_ATTACK_VMA));
    fn_equip_item = reinterpret_cast<EquipItemFn>(g_base + fn_resolve("F_EQUIP_ITEM_VMA", F_EQUIP_ITEM_VMA));
    fn_unequip = reinterpret_cast<UnequipFn>(g_base + fn_resolve("F_UNEQUIP_VMA", F_UNEQUIP_VMA));
    fn_can_equip = reinterpret_cast<CanEquipFn>(g_base + fn_resolve("F_CAN_EQUIP_VMA", F_CAN_EQUIP_VMA));
    fn_find_equip_slot = reinterpret_cast<FindEquipSlotFn>(g_base + fn_resolve("F_FIND_EQUIP_SLOT_VMA", F_FIND_EQUIP_SLOT_VMA));
    fn_get_equip_item = reinterpret_cast<GetEquipItemFn>(g_base + fn_resolve("F_GET_EQUIP_ITEM_VMA", F_GET_EQUIP_ITEM_VMA));
    fn_is_special_npc = reinterpret_cast<IsSpecialNpcFn>(g_base + fn_resolve("F_IS_SPECIAL_NPC_VMA", F_IS_SPECIAL_NPC_VMA));
    fn_learn_action = reinterpret_cast<LearnActionFn>(g_base + fn_resolve("F_LEARN_ACTION_VMA", F_LEARN_ACTION_VMA));
    fn_set_active_player = reinterpret_cast<SetActivePlayerFn>(g_base + fn_resolve("F_SET_ACTIVE_PLAYER_VMA", F_SET_ACTIVE_PLAYER_VMA));
    fn_party_swap = reinterpret_cast<PartySwapFn>(g_base + fn_resolve("F_PARTY_SWAP_VMA", F_PARTY_SWAP_VMA));
    fn_set_position = reinterpret_cast<SetPositionFn>(g_base + fn_resolve("F_SET_POSITION_VMA", F_SET_POSITION_VMA));
    fn_change_map = reinterpret_cast<ChangeMapFn>(g_base + fn_resolve("F_CHANGE_MAP_VMA", F_CHANGE_MAP_VMA));
    fn_move_as_path = reinterpret_cast<MoveAsPathFn>(g_base + fn_resolve("F_MOVE_AS_PATH_VMA", F_MOVE_AS_PATH_VMA));
    fn_char_move = reinterpret_cast<CharMoveFn>(g_base + fn_resolve("F_CHAR_MOVE_VMA", F_CHAR_MOVE_VMA));
    fn_char_pick_item_all = reinterpret_cast<CharPickItemAllFn>(g_base + fn_resolve("F_CHAR_PICK_ITEM_ALL_VMA", F_CHAR_PICK_ITEM_ALL_VMA));
    fn_char_get_area_rect = reinterpret_cast<CharGetAreaRectFn>(g_base + fn_resolve("F_CHAR_GET_AREA_RECT_VMA", F_CHAR_GET_AREA_RECT_VMA));
    fn_mem_malloc = reinterpret_cast<MemMallocFn>(g_base + fn_resolve("F_MEM_MALLOC_VMA", F_MEM_MALLOC_VMA));
    fn_notifier_add = reinterpret_cast<NotifierAddFn>(g_base + fn_resolve("F_NOTIFIER_ADD_VMA", F_NOTIFIER_ADD_VMA));
    fn_char_set_direction = reinterpret_cast<CharSetDirectionFn>(g_base + fn_resolve("F_CHAR_SET_DIRECTION_VMA", F_CHAR_SET_DIRECTION_VMA));
    fn_char_remove_path = reinterpret_cast<CharRemovePathFn>(g_base + fn_resolve("F_CHAR_REMOVE_PATH_VMA", F_CHAR_REMOVE_PATH_VMA));
    fn_map_set_focus = reinterpret_cast<MapSetFocusFn>(g_base + fn_resolve("F_MAP_SET_FOCUS_VMA", F_MAP_SET_FOCUS_VMA));
    fn_go_map_link_by_char = reinterpret_cast<GoMapLinkByCharFn>(g_base + fn_resolve("F_GAMEPLAY_GO_MAP_LINK_BY_CHAR_VMA", F_GAMEPLAY_GO_MAP_LINK_BY_CHAR_VMA));
    fn_char_set_target = reinterpret_cast<CharSetTargetFn>(g_base + fn_resolve("F_CHAR_SET_TARGET_VMA", F_CHAR_SET_TARGET_VMA));
    fn_char_stop_combat = reinterpret_cast<CharStopCombatFn>(g_base + fn_resolve("F_CHAR_STOP_COMBAT_VMA", F_CHAR_STOP_COMBAT_VMA));
    fn_consume_item = reinterpret_cast<ConsumeItemFn>(g_base + fn_resolve("F_CONSUME_ITEM_VMA", F_CONSUME_ITEM_VMA));
    fn_char_use_item_ex = reinterpret_cast<CharUseItemExFn>(g_base + fn_resolve("F_CHAR_USE_ITEM_EX_VMA", F_CHAR_USE_ITEM_EX_VMA));
    fn_remove_item_direct = reinterpret_cast<RemoveItemDirectFn>(g_base + fn_resolve("F_REMOVE_ITEM_DIRECT_VMA", F_REMOVE_ITEM_DIRECT_VMA));
    fn_include_party = reinterpret_cast<IncludePartyFn>(g_base + fn_resolve("F_INCLUDE_PARTY_VMA", F_INCLUDE_PARTY_VMA));
    fn_exclude_party = reinterpret_cast<ExcludePartyFn>(g_base + fn_resolve("F_EXCLUDE_PARTY_VMA", F_EXCLUDE_PARTY_VMA));
    fn_mercenary_release = reinterpret_cast<MercenaryReleaseFn>(g_base + fn_resolve("F_MERCENARY_RELEASE_VMA", F_MERCENARY_RELEASE_VMA));
    fn_is_use = reinterpret_cast<ItemIsUseFn>(g_base + fn_resolve("F_ITEMDATA_IS_USE_VMA", F_ITEMDATA_IS_USE_VMA));
    fn_popup_exist = reinterpret_cast<PopupStateExistFn>(g_base + fn_resolve("F_POPUPSTATE_EXIST_VMA", F_POPUPSTATE_EXIST_VMA));
    fn_networkstore_set_state = reinterpret_cast<NetworkStoreSetStateFn>(g_base + fn_resolve("F_NETWORKSTORE_SET_STATE_VMA", F_NETWORKSTORE_SET_STATE_VMA));
    fn_open_item_box = reinterpret_cast<OpenItemBoxFn>(g_base + fn_resolve("F_OPEN_ITEM_BOX_VMA", F_OPEN_ITEM_BOX_VMA));
    fn_release_sealed = reinterpret_cast<ReleaseSealedFn>(g_base + fn_resolve("F_RELEASE_SEALED_VMA", F_RELEASE_SEALED_VMA));
    fn_is_dice = reinterpret_cast<IsDiceFn>(g_base + fn_resolve("F_IS_DICE_VMA", F_IS_DICE_VMA));
    fn_is_sealed = reinterpret_cast<IsSealedFn>(g_base + fn_resolve("F_IS_SEALED_VMA", F_IS_SEALED_VMA));
    fn_is_item_box = reinterpret_cast<IsItemBoxFn>(g_base + fn_resolve("F_IS_ITEMBOX_VMA", F_IS_ITEMBOX_VMA));
    fn_make_item = reinterpret_cast<MakeItemFn>(g_base + fn_resolve("F_MAKE_ITEM_VMA", F_MAKE_ITEM_VMA));
    fn_create_item = reinterpret_cast<CreateItemFn>(g_base + fn_resolve("F_CREATE_ITEM_VMA", F_CREATE_ITEM_VMA));
    fn_control_object_get_data = reinterpret_cast<ControlObjectGetDataFn>(g_base + fn_resolve("F_CONTROL_OBJECT_GET_DATA_VMA", F_CONTROL_OBJECT_GET_DATA_VMA));
    fn_ui_equip_is_apply_stuff = reinterpret_cast<UiEquipIsApplyStuffFn>(g_base + fn_resolve("F_UIEQUIP_IS_APPLY_STUFF_VMA", F_UIEQUIP_IS_APPLY_STUFF_VMA));
    fn_ui_equip_get_item_slot_index = reinterpret_cast<UiEquipGetItemSlotIndexFn>(g_base + fn_resolve("F_UIEQUIP_GET_ITEM_SLOT_INDEX_VMA", F_UIEQUIP_GET_ITEM_SLOT_INDEX_VMA));
    fn_ui_equip_refresh_item_area = reinterpret_cast<UiEquipRefreshItemAreaFn>(g_base + fn_resolve("F_UIEQUIP_REFRESH_ITEM_AREA_VMA", F_UIEQUIP_REFRESH_ITEM_AREA_VMA));
    fn_ui_desc_set_off = reinterpret_cast<UiDescSetOffFn>(g_base + fn_resolve("F_UIDESC_SET_OFF_VMA", F_UIDESC_SET_OFF_VMA));
    fn_touch_handle_set_cursor = reinterpret_cast<TouchHandleSetCursorFn>(g_base + fn_resolve("F_TOUCHHANDLE_SET_CURSOR_VMA", F_TOUCHHANDLE_SET_CURSOR_VMA));
    fn_ui_equip_inven_item_control_event_proc = reinterpret_cast<UiEquipInvenItemControlEventProcFn>(g_base + fn_resolve("F_UIEQUIP_INVEN_ITEM_CONTROL_EVENT_PROC_VMA", F_UIEQUIP_INVEN_ITEM_CONTROL_EVENT_PROC_VMA));
    fn_item_draw_porting = reinterpret_cast<ItemDrawPortingFn>(g_base + fn_resolve("F_ITEM_DRAW_PORTING_VMA", F_ITEM_DRAW_PORTING_VMA));
    fn_make_mix = reinterpret_cast<MakeMixFn>(g_base + fn_resolve("F_MAKE_MIX_VMA", F_MAKE_MIX_VMA));
    fn_get_cost = reinterpret_cast<GetCostFn>(g_base + fn_resolve("F_GET_COST_VMA", F_GET_COST_VMA));
    fn_save_save_item = reinterpret_cast<SaveSaveItemFn>(g_base + fn_resolve("F_SAVE_SAVE_ITEM_VMA", F_SAVE_SAVE_ITEM_VMA));
    fn_save_load_item = reinterpret_cast<SaveLoadItemFn>(g_base + fn_resolve("F_SAVE_LOAD_ITEM_VMA", F_SAVE_LOAD_ITEM_VMA));
    fn_itempool_free = reinterpret_cast<ItemPoolFreeFn>(g_base + fn_resolve("F_ITEMPOOL_FREE_VMA", F_ITEMPOOL_FREE_VMA));
    fn_ctrl_create = reinterpret_cast<ControlObjectCreateFn>(g_base + fn_resolve("F_CONTROL_OBJECT_CREATE_VMA", F_CONTROL_OBJECT_CREATE_VMA));
    fn_ctrl_add = reinterpret_cast<ControlObjectAddFn>(g_base + fn_resolve("F_CONTROL_OBJECT_ADD_CONTROL_OBJECT_VMA", F_CONTROL_OBJECT_ADD_CONTROL_OBJECT_VMA));
    fn_ctrl_set_rect = reinterpret_cast<ControlObjectSetRectFn>(g_base + fn_resolve("F_CONTROL_OBJECT_SET_RECT_VMA", F_CONTROL_OBJECT_SET_RECT_VMA));
    fn_ctrl_set_event_call_type = reinterpret_cast<ControlObjectSetEventCallTypeFn>(g_base + fn_resolve("F_CONTROL_OBJECT_SET_CONTROL_EVENT_CALL_TYPE_VMA", F_CONTROL_OBJECT_SET_CONTROL_EVENT_CALL_TYPE_VMA));
    fn_ctrl_set_data = reinterpret_cast<ControlObjectSetDataFn>(g_base + fn_resolve("F_CONTROL_OBJECT_SET_DATA_VMA", F_CONTROL_OBJECT_SET_DATA_VMA));
    fn_ctrl_btn_create = reinterpret_cast<ControlButtonCreateFn>(g_base + fn_resolve("F_CONTROL_BUTTON_CREATE_VMA", F_CONTROL_BUTTON_CREATE_VMA));
    fn_ctrl_btn_set_text = reinterpret_cast<ControlButtonSetTextFn>(g_base + fn_resolve("F_CONTROL_BUTTON_SET_TEXT_VMA", F_CONTROL_BUTTON_SET_TEXT_VMA));
    fn_ctrl_btn_set_draw_type = reinterpret_cast<ControlButtonSetDrawTypeFn>(g_base + fn_resolve("F_CONTROL_BUTTON_SET_DRAW_TYPE_VMA", F_CONTROL_BUTTON_SET_DRAW_TYPE_VMA));
    fn_ctrl_btn_set_draw_id = reinterpret_cast<ControlButtonSetDrawIDFn>(g_base + fn_resolve("F_CONTROL_BUTTON_SET_DRAW_ID_VMA", F_CONTROL_BUTTON_SET_DRAW_ID_VMA));
    fn_ctrl_btn_set_draw_sub_id = reinterpret_cast<ControlButtonSetDrawSubIDFn>(g_base + fn_resolve("F_CONTROL_BUTTON_SET_DRAW_SUB_ID_VMA", F_CONTROL_BUTTON_SET_DRAW_SUB_ID_VMA));
    fn_ctrl_btn_set_draw_proc = reinterpret_cast<ControlButtonSetDrawProcFn>(g_base + fn_resolve("F_CONTROL_BUTTON_SET_DRAW_PROC_VMA", F_CONTROL_BUTTON_SET_DRAW_PROC_VMA));
    fn_ui_create_group_base_control = reinterpret_cast<UiCreateGroupBaseControlFn>(g_base + fn_resolve("F_UI_CREATE_GROUP_BASE_CONTROL_VMA", F_UI_CREATE_GROUP_BASE_CONTROL_VMA));
    fn_popup_create = reinterpret_cast<UiPopupMsgCreateFn>(g_base + fn_resolve("F_UIPOPUPMSG_CREATE_VMA", F_UIPOPUPMSG_CREATE_VMA));
    fn_popup_create_yesno = reinterpret_cast<UiPopupMsgCreateYesNoFn>(g_base + fn_resolve("F_UIPOPUPMSG_CREATE_YESNO_VMA", F_UIPOPUPMSG_CREATE_YESNO_VMA));
    fn_popup_create_from_textdata = reinterpret_cast<UiPopupMsgCreateFromTextDataFn>(g_base + fn_resolve("F_UIPOPUPMSG_CREATE_FROM_TEXTDATA_VMA", F_UIPOPUPMSG_CREATE_FROM_TEXTDATA_VMA));
    fn_popup_free = reinterpret_cast<UiPopupMsgFreeFn>(g_base + fn_resolve("F_UIPOPUPMSG_FREE_VMA", F_UIPOPUPMSG_FREE_VMA));
    fn_popupstate_push = reinterpret_cast<PopupStatePushFn>(g_base + fn_resolve("F_POPUPSTATE_PUSH_VMA", F_POPUPSTATE_PUSH_VMA));
    fn_grpx_start = reinterpret_cast<GrpxStartFn>(g_base + fn_resolve("F_GRPX_START_VMA", F_GRPX_START_VMA));
    fn_grpx_end = reinterpret_cast<GrpxEndFn>(g_base + fn_resolve("F_GRPX_END_VMA", F_GRPX_END_VMA));
    fn_grpx_fill_rect = reinterpret_cast<GrpxFillRectFn>(g_base + fn_resolve("F_GRPX_FILL_RECT_VMA", F_GRPX_FILL_RECT_VMA));
    fn_grpx_fill_rect_alpha = reinterpret_cast<GrpxFillRectAlphaFn>(g_base + fn_resolve("F_GRPX_FILL_RECT_ALPHA_VMA", F_GRPX_FILL_RECT_ALPHA_VMA));
    fn_grpx_set_font_color = reinterpret_cast<GrpxSetFontColorFn>(g_base + fn_resolve("F_GRPX_SET_FONT_COLOR_VMA", F_GRPX_SET_FONT_COLOR_VMA));
    fn_grpx_draw_string_with_font = reinterpret_cast<GrpxDrawStringWithFontFn>(g_base + fn_resolve("F_GRPX_DRAW_STRING_WITH_FONT_VMA", F_GRPX_DRAW_STRING_WITH_FONT_VMA));
    fn_grpx_set_font_color_rgb = reinterpret_cast<GrpxSetFontColorRgbFn>(g_base + fn_resolve("F_GRPX_SET_FONT_COLOR_RGB_VMA", F_GRPX_SET_FONT_COLOR_RGB_VMA));
    fn_grpx_draw_part = reinterpret_cast<GrpxDrawPartFn>(g_base + fn_resolve("F_GRPX_DRAW_PART_VMA", F_GRPX_DRAW_PART_VMA));
    fn_ui_draw_string_halign = reinterpret_cast<UiDrawStringHAlignFn>(g_base + fn_resolve("F_UI_DRAW_STRING_HALIGN_VMA", F_UI_DRAW_STRING_HALIGN_VMA));
    fn_ui_draw_string_in_width_with_font = reinterpret_cast<UiDrawStringInWidthWithFontFn>(g_base + fn_resolve("F_UI_DRAW_STRING_IN_WIDTH_WITH_FONT_VMA", F_UI_DRAW_STRING_IN_WIDTH_WITH_FONT_VMA));
    fn_mw_graphic_draw_string = reinterpret_cast<MwGraphicDrawStringFn>(g_base + fn_resolve("F_MW_GRAPHIC_DRAW_STRING_VMA", F_MW_GRAPHIC_DRAW_STRING_VMA));
    fn_grp_save_lcd = reinterpret_cast<GrpSaveLcdFn>(g_base + fn_resolve("F_GRP_SAVE_LCD_VMA", F_GRP_SAVE_LCD_VMA));
    fn_grp_restore_lcd = reinterpret_cast<GrpRestoreLcdFn>(g_base + fn_resolve("F_GRP_RESTORE_LCD_VMA", F_GRP_RESTORE_LCD_VMA));
    fn_ui_set_refresh_lcd_flag = reinterpret_cast<UiSetRefreshLcdFlagFn>(g_base + fn_resolve("F_UI_SET_REFRESH_LCD_FLAG_VMA", F_UI_SET_REFRESH_LCD_FLAG_VMA));
    fn_ui_get_refresh_lcd_flag = reinterpret_cast<UiGetRefreshLcdFlagFn>(g_base + fn_resolve("F_UI_GET_REFRESH_LCD_FLAG_VMA", F_UI_GET_REFRESH_LCD_FLAG_VMA));
    fn_calc_res_width = reinterpret_cast<CalcResolutionFn>(g_base + fn_resolve("F_CALC_RESOLUTION_WIDTH_VMA", F_CALC_RESOLUTION_WIDTH_VMA));
    fn_calc_res_height = reinterpret_cast<CalcResolutionFn>(g_base + fn_resolve("F_CALC_RESOLUTION_HEIGHT_VMA", F_CALC_RESOLUTION_HEIGHT_VMA));
    fn_imgsys_unit_load = reinterpret_cast<ImgsysUnitFn>(g_base + fn_resolve("F_IMGSYS_UNIT_LOAD_VMA", F_IMGSYS_UNIT_LOAD_VMA));
    fn_imgsys_unit_unload = reinterpret_cast<ImgsysUnitFn>(g_base + fn_resolve("F_IMGSYS_UNIT_UNLOAD_VMA", F_IMGSYS_UNIT_UNLOAD_VMA));
    fn_imgsys_get_group = reinterpret_cast<ImgsysGetGroupFn>(g_base + fn_resolve("F_IMGSYS_GET_GROUP_VMA", F_IMGSYS_GET_GROUP_VMA));
    fn_imgsys_get_loc = reinterpret_cast<ImgsysGetLocFn>(g_base + fn_resolve("F_IMGSYS_GET_LOC_VMA", F_IMGSYS_GET_LOC_VMA));
    fn_get_group_title_img_type = reinterpret_cast<GetGroupTitleImgTypeFn>(g_base + fn_resolve("F_GET_GROUP_TITLE_IMG_TYPE_VMA", F_GET_GROUP_TITLE_IMG_TYPE_VMA));
    fn_ctrl_get_count = reinterpret_cast<ControlObjectGetCountFn>(g_base + fn_resolve("F_CONTROL_OBJECT_GET_COUNT_VMA", F_CONTROL_OBJECT_GET_COUNT_VMA));
    fn_ctrl_get_child = reinterpret_cast<ControlObjectGetChildFn>(g_base + fn_resolve("F_CONTROL_OBJECT_GET_CHILD_VMA", F_CONTROL_OBJECT_GET_CHILD_VMA));
    fn_ctrl_get_data = reinterpret_cast<ControlObjectGetDataFn>(g_base + fn_resolve("F_CONTROL_OBJECT_GET_DATA_VMA", F_CONTROL_OBJECT_GET_DATA_VMA));
    fn_ctrl_set_active = reinterpret_cast<ControlObjectSetActiveFn>(g_base + fn_resolve("F_CONTROL_OBJECT_SET_ACTIVE_VMA", F_CONTROL_OBJECT_SET_ACTIVE_VMA));
    fn_ctrl_btn_draw = reinterpret_cast<ControlButtonDrawFn>(g_base + fn_resolve("F_CONTROL_BUTTON_DRAW_VMA", F_CONTROL_BUTTON_DRAW_VMA));
    if (!apply_fixed_stack_layout()) {
        g_dl_error = "fixed stack layout patch failed";
        g_base = 0;
        return false;
    }
    return true;
}

// 当前地图真实 ID（v0.4.28）：GOT 双层解引用 u32 = MAPINFOBASE 记录下标。
// 来源：MAP_Load(0x1149d4) 写 *(*(0x2f4000+0xe80))（114ae8 str w22,[x1]）。
uint32_t current_map_id() {
    if (g_base == 0) return 0;
    void** slot = reinterpret_cast<void**>(g_base + G_CUR_MAP_ID_GOT_VMA);
    if (slot == nullptr || *slot == nullptr) return 0;
    return *reinterpret_cast<uint32_t*>(*slot);
}
