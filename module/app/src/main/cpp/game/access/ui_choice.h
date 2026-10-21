#pragma once

#include <cstdint>

// 游戏层：原生 UICHOICE 选择框语义访问（ARCHITECTURE.md §5 版本耦合唯一收口）。
// state 表选择框条目 id 与按钮 ExecuteProc 槽站点的 VMA/符号细节收口在本层；
// 插件层只调用语义函数，不出现裸地址/符号头。

namespace qol::game {

// 选择框（F_PANEL_CHOICE_ENTER）在 PopupState 表中的 state id；未就绪/未命中 → -1。
int ui_choice_state_id();

// 按钮 ExecuteProc 槽站点（GOT slot + 安装前期望的原函数入口）。
// 供 kPtrSlot 声明经 game::ptr_slot_site("ptrslot:g_uichoice_button_list_exe_got") 解析。
bool ui_choice_button_slot_target(void** out_slot, uintptr_t* out_expected);

bool ui_choice_write_items(const char* const* items, uint32_t count);
bool ui_choice_write_title(const char* title);
bool ui_choice_write_focus(int32_t index);
bool ui_choice_push();
bool ui_choice_close_popup();
bool ui_choice_play_sound(int32_t sound_id);
bool ui_choice_selected_index(int32_t* out_index);
bool ui_choice_text_owns(const char* const* items, uint32_t count);

// Module-choice first-frame de-dim primitives (Scene_Draw_POPUP_SC_CHOICE hook).
// Each maps to one game primitive; the plugin wrapper owns the sequence.
bool ui_choice_refresh_pending(bool* out_pending);
bool ui_choice_save_lcd();
bool ui_choice_clear_refresh_flag();
bool ui_choice_begin_frame();
bool ui_choice_draw();
bool ui_choice_end_frame();

}  // namespace qol::game
