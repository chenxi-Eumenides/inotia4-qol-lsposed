#pragma once

#include <cstddef>
#include <cstdint>

// 游戏层：合成器（UIMix）界面原语与符号收口（ARCHITECTURE.md §5）。
//
// UIMix 控件槽读写、填入格操作、界面动作调用与符号常量全部收口在本层；features 只调用
// 语义函数，不出现裸偏移、不直接 include 符号头。本层不做配方判定（结果归 custom_recipe），
// 也不做功能开关判定（总开关由调用方/能力契约处理）。
//
// 线程模型：全部函数只在游戏主线程（UI 回调 / wrapper）调用；只读写游戏内存，不加锁。
// 一律 fail-safe：g_uimix 未就绪时返回 nullptr / 0 / -1，不写内存、不崩溃。

namespace qol::game {

// 填入格数量（`[g_uimix + UIMIX_SLOT_STUFF_GROUP]` 组的前 3 个子控件；经
// ControlObject_GetChild 取，不是 `+0xc8 + i*8` 直索引）。
constexpr size_t kCraftStuffSlotCount = 3;

// ---- UIMix 状态读取 ----

// `[g_uimix + offset]`；未就绪返回 nullptr。
void* craft_slot(size_t offset);

// 当前 mixType（`[+UIMIX_SLOT_MIXTYPE]` u32）。未就绪返回 0。
uint32_t craft_mix_type();

// 当前合成类型（`[+UIMIX_SLOT_TYPE]` u8；0=药水 1=宝石 2=打孔 3=混沌 4=传说）。未就绪返回 -1。
int64_t craft_ui_type();

// UIMix_GetType 原函数读取的类型（与 craft_ui_type 同源；未解析返回 -1）。
int64_t craft_get_ui_type();

// 各 type 已选配方下标（`[+UIMIX_SLOT_SELECTED_RECIPE_BASE + type*8]` i64）。
int64_t craft_selected_recipe_at(int64_t type);
void craft_set_selected_recipe_at(int64_t type, int64_t value);

// 目标槽（`[+UIMIX_SLOT_TARGET_ITEM]`）控件与其当前物品。
void* craft_target_slot_control();
void* craft_target_slot_item();
void craft_set_target_slot_item(void* item);

// 面板状态字节（`[+UIMIX_SLOT_STATE]` u8）；未就绪返回 0。
uint8_t craft_panel_state();

// ---- 填入格 ----

// 第 index 个填入格的 ControlItem 控件。越界或未就绪返回 nullptr。
void* craft_stuff_slot_control(size_t index);

// 第 index 填入格的当前物品（空槽 = nullptr）。
void* craft_stuff_item(size_t index);

// 写入第 index 填入格（item 为 nullptr 即清空该格）。
void craft_set_stuff_item(size_t index, void* item);

// 读 3 格占用情况；返回已放物品的格数。out 须能容纳 kCraftStuffSlotCount 个元素。
int craft_read_stuff_filled(bool out[kCraftStuffSlotCount]);

// 读 3 格物品类别（`item + I_TYPE` 的 bits6-15）；空格填 0。返回已放物品的格数。
int craft_read_stuff_categories(uint16_t out[kCraftStuffSlotCount]);

// 当前选中填入格下标（`[+UIMIX_SLOT_SELECTED_STUFF]` i64；-1 = 未选中）。未就绪返回 -1。
int64_t craft_selected_stuff_index();

// 写入选中填入格下标（传 -1 即取消选中）。
void craft_select_stuff_slot(int64_t index);

// 读 3 格占用后选中索引最小的空格；全满则设为 -1。返回选中的下标。
int craft_select_first_empty_stuff_slot();

// 材料需求条目数（`[+UIMIX_SLOT_STUFF_NUM]` u32）；未就绪返回 0。
uint32_t craft_stuff_slot_count();

// 写材料需求数（stuffList[index] + UIMIX_STUFF_ENTRY_NEED，u16）；越界/未就绪不动作。
// 该字段是扣料（UseStuff 读 +6）与显示（UIMix_Draw 每帧读 +6）的唯一真源（§3.9）。
void craft_write_stuff_need(size_t index, uint16_t need);

// 合成费用 `[+UIMIX_SLOT_COST]` 写 0（覆盖原函数 CAL_Calculate 结果；显示与实扣同时为 0）。
void craft_set_cost_zero();

// 背包网格当前选中物（`[+UIMIX_SLOT_ITEM_GROUP]` 组 cursor → GetData → `*data`）。
void* craft_selected_inven_item();

// ---- 界面动作 ----

void craft_init_mixing_state();       // UIMix_InitMixingState（不清填入格）
void craft_reset_stuff_item_control();  // UIMix_ResetStuffItemControl（清空填入格）
void craft_refresh_inven_items();     // UIMix_RefreshInvenItem（按当前袋号刷新）
void craft_set_ui_type(int64_t type); // UIMix_SetType（type 仅限 0..4）
void craft_play_sound(int16_t sound_id);
void craft_show_text(uint32_t word_id);  // 原生单项提示框
void craft_desc_set_off();               // UI_Desc_SetOff（与原版放料前一致）

// UIPopupMsg_CreateYesNoFromTextData(text_id, 0, 0, ok, cancel, nullptr)：
// 3 格合成确认框（ok/cancel 为原版回调槽，本模块传自有函数指针）。未解析返回 false。
bool craft_show_yesno_from_textdata(uint32_t text_id, void* ok, void* cancel);

// ---- 游戏读取（供结果层/界面层构造契约入参）----

// 主角等级（PARTY_GetMember(0) + C_LEVEL int8）；读不到返回 0。
int craft_character_level();

// 某类别当前持有总数（INVEN_GetItemCount）；未解析/非正返回 0。
int craft_category_held_count(uint16_t category);

// MATH_GetRandom(min,max) 闭区间随机；未解析返回 lo。
int craft_math_get_random(int lo, int hi);

}  // namespace qol::game
