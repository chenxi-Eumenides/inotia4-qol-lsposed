#pragma once

#include <cstddef>
#include <cstdint>

#include "feature/patch/native_inventory_hook.h"  // NativeHookFunType

// 合成器界面能力层（craft_ui）：UIMix 合成器控件读写与界面动作的**唯一实现处**。
//
// 分层约定（用户裁决 2026-09-16，「界面归界面，结果归结果」）：
//   · 本层只管**界面**——填入了什么、选中哪一格、清空后恢复到哪个格、弹什么提示；
//     **不含任何配方知识**（不知道什么是宝石、哪条配方成立）。
//   · 配方层（feature/custom_recipe）只管**结果**——给定三格回答「结果枚举 + 产物类别 +
//     要扣什么材料」；它**不知道 UI 存在**，一行弹窗或选中代码都没有。
//   · 宝石合成与自定义配方**共用**本层与同一个开关（gemcraftEnabled 已删除）。
//
// 依赖方向单向：调用方（配方层 / 合成器流程）→ craft_ui；本层不反向依赖任何 feature。
//
// 线程模型：全部函数只在游戏主线程（UI 回调 / wrapper）调用；只读写游戏内存，不加锁。
// 一律 fail-safe：g_uimix 未就绪时返回 nullptr / 0 / -1，不写内存、不崩溃。
namespace craft_ui {

// 填入格数量（`[g_uimix + UIMIX_SLOT_STUFF_GROUP]` 组的前 3 个子控件；必须经
// ControlObject_GetChild 取，**不是** `+0xc8 + i*8` 直索引）。
constexpr size_t kStuffSlotCount = 3;

// ---- UIMix 状态读取 ----

// `[g_uimix + offset]`。g_uimix 未就绪返回 nullptr。
void* slot(size_t offset);

// 当前 mixType（`[+UIMIX_SLOT_MIXTYPE]` u32）。未就绪返回 0。
uint32_t mix_type();

// 当前合成类型（`[+UIMIX_SLOT_TYPE]` u8；0=药水 1=宝石 2=打孔 3=混沌 4=传说）。未就绪返回 -1。
int64_t ui_type();

// 各 type 已选配方下标（`[+UIMIX_SLOT_SELECTED_RECIPE_BASE + type*8]` i64）。
int64_t selected_recipe_at(int64_t type);
void set_selected_recipe_at(int64_t type, int64_t value);

// 目标槽（`[+UIMIX_SLOT_TARGET_ITEM]`）控件与其当前物品。
void* target_slot_control();
void* target_slot_item();
void set_target_slot_item(void* item);

// ---- 填入格 ----

// 第 index 个填入格的 ControlItem 控件。越界或未就绪返回 nullptr。
void* stuff_slot_control(size_t index);

// 第 index 填入格的当前物品（空槽 = nullptr）。
void* stuff_item(size_t index);

// 写入第 index 填入格（item 为 nullptr 即清空该格）。
void set_stuff_item(size_t index, void* item);

// 读 3 格占用情况；返回已放物品的格数。out 须能容纳 kStuffSlotCount 个元素。
int read_stuff_filled(bool out[kStuffSlotCount]);

// 读 3 格物品类别（`item + I_TYPE` 的 bits6-15）；**空格填 0**。返回已放物品的格数。
int read_stuff_categories(uint16_t out[kStuffSlotCount]);

// ---- 选中格 / 背包侧 ----

// 当前选中填入格下标（`[+UIMIX_SLOT_SELECTED_STUFF]` i64；-1 = 未选中）。未就绪返回 -1。
int64_t selected_stuff_index();

// 写入选中填入格下标（传 -1 即取消选中）。
void select_stuff_slot(int64_t index);

// 读 3 格占用后选中**索引最小的空格**；全满则设为 -1（未选中）。返回选中的下标。
// 用途：进入合成视图、放料之后、以及**清空填入格之后**（合成失败的统一收尾）——
// 否则选中下标被清成 -1，玩家点背包物品没有落点，只能先手动点一次格子。
int select_first_empty_stuff_slot();

// 背包网格当前选中物（`[+UIMIX_SLOT_ITEM_GROUP]` 组 cursor → GetData → `*data`）。
void* selected_inven_item();

// ---- 界面动作 ----

// UIMix_InitMixingState：依 mixType 重算材料需求列表与费用（**不清**填入格）。
void init_mixing_state();

// UIMix_ResetStuffItemControl：清空填入格。
void reset_stuff_item_control();

// UIMix_RefreshInvenItem：按当前袋号刷新背包网格。
void refresh_inven_items();

// 清空填入格 + 刷新背包网格（不含 InitMixingState、不含音效与提示）。
// 合成失败后的统一收尾，让玩家立刻重试。
void reset_stuff_and_refresh();

// UIMix_SetType：写 `[+UIMIX_SLOT_TYPE]`。
// **type >= 5 直接返回**——原版对 type>=5 走 ResetActiveControl 未初始化寄存器路径。
void set_ui_type(int64_t type);

// SOUNDSYSTEM_Play。
void play_sound(int16_t sound_id);

// ---- 提示 ----

// 原生单项提示框（`fn_popup_create_ok_from_textdata(word_id, 0, 0, 0)`）。
void show_text(uint32_t word_id);

// ---- hook ----

// 安装一个函数级 hook。rc != 0 或 backup 为空即失败（返回 false，调用方 fail-closed）。
bool install_one(NativeHookFunType hook, uintptr_t target, void* replacement, void** backup,
                 const char* name);

}  // namespace craft_ui
