#pragma once

#include "game_symbols.h"

#include <jni.h>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

// 符号解析层：从 /proc/self/maps 定位 libgame.so 基址，计算全部符号运行时地址。
// 不用 dlopen/dlsym：Android linker namespace 隔离下 dlopen 会加载独立副本，读不到游戏数据。

extern uintptr_t g_base;
// 检查游戏地址区间是否落在当前进程具有指定权限的映射中。
// 游戏 UI 控件会在跨帧重建，调用其 getter 前必须先避免解引用失效控件。
extern bool game_memory_accessible(const void* address, size_t size, char permission);
extern JavaVM* g_jvm();   // JNI_OnLoad 缓存（gamebridge.cpp），native 反调 Kotlin 用
extern void* g_money;
extern void* g_map_id;
extern uint32_t current_map_id();
extern void* g_party;
extern void* g_active_quest;
extern void* g_inven;
extern void* g_main_merc_slot;
extern void* g_prev_state;
extern void* g_state;
extern void* g_gamestate;
extern void* g_initstate;
extern void* g_popup_on;
extern void* g_mainmenu_draw;
extern void* g_popup_stack;
extern void* g_player_active;
extern void* g_uimix;

extern GetMoneyFn fn_get_money;
extern GetMemberFn fn_get_member;
extern GetPartySizeFn fn_get_party_size;
// ⚠️ 禁止在非游戏线程调用：CHAR_GetAttr(0xdfd18) attr=0x1e 分支在 HP>maxHP 时会写回 HP
// （HP 钳制写副作用），其它分支亦有写；JSON/预取线程请改读属性缓存字段（char_max_hp/mp）
// 或走游戏线程缓存。
extern GetAttrFn fn_get_attr;
extern GetEquipFn fn_get_equip;
// CHAR_GetExperience(0xd9b54)：`ldr w0,[x0,#0x318]` 纯读，跨线程安全。
extern GetExpFn fn_get_exp;
// ⚠️ 禁止在非游戏线程调用：CHAR_GetNextExperience(0xd9b68) 在 [ch+0x320]==0 时用 CAL_Calculate
// 现算并写回该字段（写操作），且 CHAR_SetLevel 会清零失效；JSON 请用 char_next_exp_cached()。
extern GetExpFn fn_get_next_exp;
extern GetRarityFn fn_get_rarity;
extern GetBagSizeFn fn_get_bag_size;
extern GetEmptyBagSlotFn fn_get_empty_bag_slot;
extern IsEmptyBagFn fn_is_empty_bag;
extern IsHavingEmptySlotFn fn_is_having_empty_slot;
extern GetBitFn fn_get_bit;
extern GetCumulateCountFn fn_get_cumulate_count;
extern GetItemStatFn fn_get_damage;
extern GetItemStatFn fn_get_defense;
// ⚠️ CHAR_GetStat(0xdf8d0) 经 CHAR_GetStatSub(0xdf888) 在动态派生脏位（[ch+0x270] bit i）置位时
// 会调 CHAR_CalculateStatus 重算并写回 [ch+0x266]/SV，属写操作，禁止非游戏线程调用；
// JSON 请用 char_stat_total() 直读。
extern GetAttrFn2 fn_get_stat;
// CHAR_GetStatBase(0xdb9e4) 为纯读（add + ldrsb），可跨线程调用。
extern GetAttrFn2 fn_get_stat_base;
// CHAR_GetStatBonus(0xdb9fc) 为纯读（add + ldrsb），可跨线程调用。
extern GetAttrFn2 fn_get_stat_bonus;
extern GetStatusPointFn fn_get_status_point;
extern GetStatMainFn fn_get_stat_main;
extern SetStatMainFn fn_set_stat_main;
extern SetStatBaseFn fn_set_stat_base;
extern RollStatusDiceFn fn_status_dice_roll;
extern PutJewelFn fn_put_jewel;
extern IsJewelFn fn_is_jewel;
extern EnchantItemFn fn_enchant_item;
extern IsEnchantScrollFn fn_is_enchant_scroll;
extern SaveIsOkFn fn_save_is_ok;
extern CharInitializeStatusFn fn_char_initialize_status;
extern CharInitializeSkillFn fn_char_initialize_skill;
extern CharSetActionIdFn fn_char_set_action_id;
extern CharGetEnemyTargetFn fn_char_get_enemy_target;
extern QuestSystemFindFn fn_questsystem_find;
extern QuestSystemRemoveSlotFn fn_questsystem_remove_slot;
extern SaveFn fn_save;
extern SaveGetSaveSlotFn fn_save_get_save_slot;
extern SaveLoadSaveSlotFn fn_save_load_save_slot;
extern UiSetPopupProcessInfoFn fn_ui_set_popup_process_info;
extern GameStartResumeGameFn fn_game_start_resume_game;
extern SaveCreateSaveSlotFn fn_save_create_save_slot;
extern SaveslotGetHeroFn fn_saveslot_get_hero;
extern StateSetFn fn_state_set;
extern StateNextStartProcessFn fn_state_next_start_process;
extern GameExitSaveSlotSelectCharFn fn_game_exit_save_slot_select_char;
extern SelectCharacterStartGameFn fn_select_character_start_game;
extern TutorialStartFn fn_tutorial_start;
extern SaveGetSaveFileNameFn fn_save_get_save_file_name;
extern CsFsRemoveFn fn_cs_fs_remove;
// ---- 存档文件加解密链（save-export 存档管理器）----
extern HubSaveGetKeyFn fn_hub_save_get_key;
extern SaveLoadDataFn fn_save_load_data;
extern MemFreeFn fn_mem_free;
extern EncryptProcess2Fn fn_encrypt_process2;
extern GamestateSetStateFn fn_gamestate_set_state;
extern GameExitFn fn_game_exit;
extern UinpcInitFn fn_uinpc_init;
extern NpcSystemCheckFunctionDisplayFn fn_check_function_display;
extern UinpcExeTaskFn fn_uinpc_exe_current_task;
extern UinpcQuestButtonOkExeFn fn_uinpc_quest_button_ok_exe;
extern NpctasklistMakeDlgFn fn_npctasklist_make_dlg;
extern PlayerCheckNearNpcFn fn_player_check_near_npc;
extern GetSkillUsageFn fn_get_skill_usage;
extern SetSkillUsageFn fn_set_skill_usage;
extern GetNameFn fn_get_name;
extern GetActMaxLevelFn fn_get_act_max_level;
extern FindMercSlotFn fn_find_merc_slot;
extern SearchPathFn fn_search_path;
extern EvtSetStateFn fn_evt_set_state;
extern TextctrlMoveNextPageFn fn_textctrl_move_next_page;
extern KeySetCodeFn fn_key_set_code;
extern IntVoidFn fn_wipeout_button_revive;
extern IntVoidFn fn_wipeout_button_special_revive;
extern IntVoidFn fn_wipeout_button_gameover;
extern IntVoidFn fn_event_button_ok_exe;
extern IntVoidFn fn_event_button_skip_exe;
extern IntIntFn fn_evtsystem_do_check_all_event;
extern IntVoidFn fn_tutorial_getstate;
extern SetMoneyFn fn_set_money;
extern AddMoneyFn fn_add_money;
extern AddMoneyFn fn_minus_money;
extern FindItemFn fn_find_item;
extern HaveItemFn fn_have_item;
extern GetItemCountFn fn_get_item_count;
extern InvenFindItemSlotFn fn_inven_find_item_slot;
extern InvenCalculateEmptySlotCountForSaveFn fn_inven_calculate_empty_slot_count_for_save;
extern InvenGetEmptySaveSlotExFn fn_inven_get_empty_save_slot_ex;
extern InvenGetNeededSaveSlotExFn fn_inven_get_needed_save_slot_ex;
extern InvenGetCumulateSaveSlotExFn fn_inven_get_cumulate_save_slot_ex;
extern RemoveItemFn fn_remove_item;
extern ItemGetPriceFn fn_item_get_price;
extern ItemGetSellPriceFn fn_item_get_sell_price;
extern IntIntFn fn_item_is_no_sell;
extern ItemGetAbilityLevelFn fn_item_get_ability_level;
extern ItemGetBuyPriceFn fn_item_get_buy_price;
extern ItemSystemGetOptionValueFn fn_item_system_get_option_value;
extern ItemSystemGetJewelOptionValueFn fn_item_system_get_jewel_option_value;
extern InvenFindSaveSlotFn fn_inven_find_save_slot;
extern InvenSaveItemFn fn_inven_save_item;
extern InvenSaveItemDirectFn fn_inven_save_item_direct;
extern InvenSaveItemOnEmptyFn fn_inven_save_item_on_empty;
extern DealSystemFindSaleByIdFn fn_dealsystem_find_sale_by_id;
extern InvenMoveItemFn fn_inven_move_item;
extern ItemSystemDivideFn fn_item_system_divide;
extern SetExpFn fn_set_exp;
extern SetLevelFn fn_set_level;
extern AddExpFn fn_add_exp;
extern SetStatusPointFn fn_set_status_point;
extern SetAutoAttackFn fn_set_auto_attack;
extern EquipItemFn fn_equip_item;
extern EquipItemFromInvenToSlotFn fn_equip_item_from_inven_to_slot;
extern UnequipFn fn_unequip;
extern CanEquipFn fn_can_equip;
extern FindEquipSlotFn fn_find_equip_slot;
extern GetEquipItemFn fn_get_equip_item;
extern SetEquipItemFn fn_set_equip_item;
extern IsSpecialNpcFn fn_is_special_npc;
extern LearnActionFn fn_learn_action;
extern SetActivePlayerFn fn_set_active_player;
extern PartySwapFn fn_party_swap;
extern SetPositionFn fn_set_position;
extern ChangeMapFn fn_change_map;

extern MoveAsPathFn fn_move_as_path;
extern CharMoveFn fn_char_move;
extern CharPickItemAllFn fn_char_pick_item_all;
extern CharGetAreaRectFn fn_char_get_area_rect;
extern MemMallocFn fn_mem_malloc;
extern NotifierAddFn fn_notifier_add;
extern CharSetDirectionFn fn_char_set_direction;
extern CharRemovePathFn fn_char_remove_path;
extern MapSetFocusFn fn_map_set_focus;
extern GoMapLinkByCharFn fn_go_map_link_by_char;
extern CharSetTargetFn fn_char_set_target;
extern CharStopCombatFn fn_char_stop_combat;
extern ConsumeItemFn fn_consume_item;
extern CharUseItemExFn fn_char_use_item_ex;
extern CharProcessShortcutFn fn_char_process_shortcut;
extern RemoveItemDirectFn fn_remove_item_direct;
extern IncludePartyFn fn_include_party;
extern ExcludePartyFn fn_exclude_party;
extern MercenaryReleaseFn fn_mercenary_release;
extern ItemIsUseFn fn_is_use;
// ---- 佣兵徽章使用链（R-63）----
extern ItemSystemIsMercenarySealFn fn_is_mercenary_seal;
extern MercenarySystemIsEmptyManagerSlotFn fn_mercenary_is_empty_manager_slot;
extern MercenarySystemMakeMercenaryFn fn_mercenary_make_mercenary;
extern PopupStateExistFn fn_popup_exist;
extern NetworkStoreSetStateFn fn_networkstore_set_state;
extern OpenItemBoxFn fn_open_item_box;
extern ReleaseSealedFn fn_release_sealed;
extern IsDiceFn fn_is_dice;
extern IsSealedFn fn_is_sealed;
extern IsItemBoxFn fn_is_item_box;
extern MakeItemFn fn_make_item;
extern CreateItemFn fn_create_item;
// UIEquip 背包面板控件（move-merge v0.6.8）
extern ControlObjectGetDataFn fn_control_object_get_data;
extern ControlObjectGetCursorIndexFn fn_control_object_get_cursor_index; // 控件光标索引（UIEquip_OKDestroyItem 背包分支取槽）
extern UiEquipIsApplyStuffFn fn_ui_equip_is_apply_stuff;
extern UiEquipApplyStuffFn fn_ui_equip_apply_stuff;
extern UiEquipGetItemSlotIndexFn fn_ui_equip_get_item_slot_index;
extern UiEquipRefreshItemAreaFn fn_ui_equip_refresh_item_area;
extern UiEquipRefreshBagAreaFn fn_ui_equip_refresh_bag_area;
extern UiStoreRefreshInvenBagFn fn_ui_store_refresh_inven_bag;
extern UiStoreRefreshInvenItemFn fn_ui_store_refresh_inven_item;
extern UiMixRefreshInvenItemFn fn_ui_mix_refresh_inven_item;
// ---- UIMix 宝石合成操作优化（gem-craft-optimization 阶段1）----
extern UIMixGetTypeFn fn_uimix_get_type;
extern UIMixButtonInvenItemSelectExeFn fn_uimix_button_inven_item_select_exe;
extern UIMixButtonMenuListExeFn fn_uimix_button_menu_list_exe;
extern UIMixButtonRecipeExeFn fn_uimix_button_recipe_exe;
extern UIMixInitMixingStateFn fn_uimix_init_mixing_state;
extern UIMixResetStuffItemControlFn fn_uimix_reset_stuff_item_control;
extern ControlObjectGetCursorFn fn_control_object_get_cursor;
extern UiPopupMsgCreateOkFromTextDataFn fn_popup_create_ok_from_textdata;
extern UiEquipUpdateCharEquipFn fn_ui_equip_update_char_equip;
extern UiDescSetOffFn fn_ui_desc_set_off;
extern UiDescGetDataFn fn_ui_desc_get_data;
extern UiEquipMakeDescFn fn_ui_equip_make_desc;
// P3 扩展袋切换音效（原版袋按钮同款：SOUNDSYSTEM_Play(0x11)）
typedef void (*SoundSystemPlayFn)(int16_t id);
extern SoundSystemPlayFn fn_sound_system_play;
extern uint8_t* g_snd_fx;
// P3 控件级投影（SetItem/SetShow typedef 在 game_symbols.h:692-694 已有）
extern ControlItemSetItemFn fn_control_item_set_item;
extern ControlObjectSetActiveFn fn_control_object_set_active;
extern ControlObjectSetShowFn fn_control_object_set_show;
extern ControlObjectGetChildFn fn_control_object_get_child;
// 注意：ControlObject_GetAbsoluteRect(0x9e748) 用 x8 sret 出参，禁止从 C++ 直接调用
// （16B 结构体走 x0/x1 返回，clang 不设 x8 → GetRelativeRect stp [x8] 写垃圾地址 →
// SEGV_ACCERR，真机实测 + docs/system/ui.md §6 第 3 坑）。取 rect 用手工父链读
// （ctrl_abs_point 模式，见 game_ui_custom.cpp）或 install 时缓存。
typedef void (*TouchHandleDeleteControlFn)(void* ctrl);
extern TouchHandleDeleteControlFn fn_touch_handle_delete_control;
extern TouchHandleResetMovingControlFn fn_touch_handle_reset_moving_control;
extern TouchHandleResetSelectedControlFn fn_touch_handle_reset_selected_control;
typedef void (*ControlObjectSetControlProcFn)(void* ctrl, void* proc);
extern ControlObjectSetControlProcFn fn_control_object_set_control_proc;
typedef void (*ControlObjectSetUserTypeFn)(void* ctrl, uint32_t type);
extern ControlObjectSetUserTypeFn fn_control_object_set_user_type;
typedef void (*TouchHandleUnuseControlEventMoveFn)(void* ctrl);
extern TouchHandleUnuseControlEventMoveFn fn_touch_handle_unuse_control_event_move;
typedef uint32_t (*ControlObjectGetUserTypeFn)(void* ctrl);
extern ControlObjectGetUserTypeFn fn_control_object_get_user_type;extern TouchHandleSetCursorFn fn_touch_handle_set_cursor;
extern UiEquipInvenItemControlEventProcFn fn_ui_equip_inven_item_control_event_proc;
extern ItemDrawPortingFn fn_item_draw_porting;
extern MakeMixFn fn_make_mix;
extern GetCostFn fn_get_cost;
// 物品序列化（lossless 扩展背包移动，v0.7.0）
extern SaveSaveItemFn fn_save_save_item;
extern SaveLoadItemFn fn_save_load_item;
extern ItemPoolFreeFn fn_itempool_free;
// ---- UI 实验函数指针（ui-exp v0.6.7）----
extern ControlObjectCreateFn fn_ctrl_create;
extern ControlObjectAddFn fn_ctrl_add;
extern ControlObjectSetRectFn fn_ctrl_set_rect;
extern ControlObjectSetEventCallTypeFn fn_ctrl_set_event_call_type;
extern ControlObjectSetDataFn fn_ctrl_set_data;
extern ControlButtonCreateFn fn_ctrl_btn_create;
extern ControlButtonSetTextFn fn_ctrl_btn_set_text;
extern ControlButtonSetDrawTypeFn fn_ctrl_btn_set_draw_type;
extern ControlButtonSetDrawIDFn fn_ctrl_btn_set_draw_id;
extern ControlButtonSetDrawSubIDFn fn_ctrl_btn_set_draw_sub_id;
extern ControlButtonSetDrawProcFn fn_ctrl_btn_set_draw_proc;
extern UiCreateGroupBaseControlFn fn_ui_create_group_base_control;
extern UiPopupMsgCreateFn fn_popup_create;
extern UiPopupMsgCreateYesNoFn fn_popup_create_yesno;
extern UiPopupMsgCreateYesNoFromTextDataFn fn_popup_create_yesno_from_textdata;
extern UiPopupMsgCreateFromTextDataFn fn_popup_create_from_textdata;
extern UiPopupMsgFreeFn fn_popup_free;
extern PopupStatePushFn fn_popupstate_push;
// ---- UI 绘制原语函数指针（ui-settings v0.6.9）----
extern MapDrawBaseFn fn_map_drawbase;
extern GrpxStartFn fn_grpx_start;
extern GrpxEndFn fn_grpx_end;
extern GrpxFillRectFn fn_grpx_fill_rect;
extern GrpxFillRectAlphaFn fn_grpx_fill_rect_alpha;
extern GrpxSetFontColorFn fn_grpx_set_font_color;
extern GrpxDrawStringWithFontFn fn_grpx_draw_string_with_font;
extern GrpxSetFontColorRgbFn fn_grpx_set_font_color_rgb;
extern GrpxDrawPartFn fn_grpx_draw_part;
extern UiDrawStringHAlignFn fn_ui_draw_string_halign;
extern UiDrawStringInWidthWithFontFn fn_ui_draw_string_in_width_with_font;
extern MwGraphicDrawStringFn fn_mw_graphic_draw_string;
extern GrpSaveLcdFn fn_grp_save_lcd;
extern GrpRestoreLcdFn fn_grp_restore_lcd;
extern UiSetRefreshLcdFlagFn fn_ui_set_refresh_lcd_flag;
extern UiGetRefreshLcdFlagFn fn_ui_get_refresh_lcd_flag;
extern CalcResolutionFn fn_calc_res_width;
extern CalcResolutionFn fn_calc_res_height;
extern ImgsysUnitFn fn_imgsys_unit_load;
extern ImgsysUnitFn fn_imgsys_unit_unload;
extern ImgsysGetGroupFn fn_imgsys_get_group;
extern ImgsysGetLocFn fn_imgsys_get_loc;
extern GetGroupTitleImgTypeFn fn_get_group_title_img_type;
extern ControlObjectGetCountFn fn_ctrl_get_count;
extern ControlObjectGetChildFn fn_ctrl_get_child;
extern ControlObjectGetDataFn fn_ctrl_get_data;
extern ControlObjectSetActiveFn fn_ctrl_set_active;
extern ControlButtonDrawFn fn_ctrl_btn_draw;

extern std::vector<std::pair<const char*, bool>> g_symbol_report;
extern std::string g_dl_error;
extern std::string g_lib_path;

// 函数符号解析：返回相对 g_base 的偏移（运行时地址 = g_base + 返回值 + 函数内 offset）。
// 供 game_patch 计算 patch 点绝对地址（禁止裸地址）。
uintptr_t fn_resolve(const char* macro_name, uintptr_t vma);

bool bridge_init();
bool bridge_ready();
