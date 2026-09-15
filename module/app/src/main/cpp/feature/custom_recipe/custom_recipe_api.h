#pragma once

#include <cstdint>

#include "feature/custom_recipe/custom_recipe_catalog.h"

// 自定义配方「结果层」（custom-craft-recipe）。
//
// 分层（用户裁定 2026-09-16：**界面归界面，结果归结果**）：
//   · 本层只回答「这 3 格是什么结果 / 该产出什么 / 该扣什么」，**不认识界面** ——
//     不弹窗、不改选中格、不碰控件、不播声音，一行 UI 动作代码都没有。
//   · 界面层 feature/craft_ui 负责全部 UIMix 控件操作与呈现，按这里回传的枚举决定弹什么。
// 依赖方向：craft_ui → custom_recipe（单向，问结果）。本层可调 craft_ui 的**只读/纯状态**
// 原语（读格、读 mixType、读选中格），但绝不调它的呈现与交互动作。
// 线程模型：全部在游戏主线程（UIMix 事件路径）调用。

namespace custom_recipe {

// 放料（3 格隐式配方）结果。
enum class PlaceOutcome : uint8_t {
    kNotMine,        // 不是 3 格模式 → 界面转调原版
    kPlaced,         // 已放入；界面随即选中下一空格
    kAlreadyPlaced,  // 不可堆叠物品重复放置 → 界面弹 99
    kNotEnoughHeld,  // 可堆叠：类别持有总数不足 → 界面弹 94（不清空已填格）
};

// 合成键前置（3 格隐式配方）结果。
enum class PrepareOutcome : uint8_t {
    kNotMine,         // 不是 3 格模式 → 界面转调原版
    kNoMatch,         // 三格不匹配任何配方 → 界面弹 94 + 清空 + 补选中
    kConfirmPending,  // 已置待合成条目 → 界面弹确认框（确认后调 three_slot_execute）
};

// 合成执行（确认后）结果。
enum class CraftOutcome : uint8_t {
    kOk,             // 成功 → 界面走成功收尾（音效 + 弹 106 + 清空 + 补选中）
    kStaleSlots,     // 确认框停留期间第 1 格被改动 → 界面弹 94 + 清空 + 补选中
    kNotEnoughHeld,  // 扣料前库存复核失败，**未消耗** → 界面弹 94（保留已填格便于补料重试）
    kNoBagSpace,     // 产物创建/入包失败（材料已扣）→ 界面弹 5 + 清空 + 补选中
};

// kJewelTierUp（原版「宝石强化」条目）合成键前置结果。
enum class CraftGate : uint8_t {
    kPass,         // 放行 → 界面转调原版
    kNotJewel,     // 目标槽非宝石 → 界面弹 97
    kAlreadyGold,  // 已达金档 → 界面弹 105
};

// 当前是否处于 3 格隐式配方模式（总开关 + 宝石强化页 + mixType 命中 kThreeSlotCraft）。
bool three_slot_mode_active();

// 模块配方（注入记录）借用的 UI type；-1 = 该 mixType 不是模块配方。
int64_t form_ui_type_for_mix_type(uint32_t mix_type);

// 放第 slot_index 格。先决：three_slot_mode_active() 为真。
PlaceOutcome three_slot_place(int64_t slot_index);

// 合成键前置。命中时内部记录待合成条目，等界面弹确认框。
PrepareOutcome three_slot_prepare();

// 确认框回调体：扣料 + 产物 + 入包。
CraftOutcome three_slot_execute();

// kJewelTierUp 的合成键前置判定。
CraftGate craft_gate();

// 目标槽物品是否为宝石（kJewelTierUp 放料后纠正用）。
bool is_jewel_item(void* item);

// kJewelTierUp 放料后写入材料需求数（按宝石档位 + 角色等级）。
void apply_material_count_for(const Def* def, void* jewel);

// kJewelTierUp 的产物改写（读改写宝石数值位）。
// 返回 <0 = 非本层配方（界面转调原版）；0 = 已改写；>0 = 失败。
int make_item(int32_t mix_type, void** out_item);

// 描述缓冲是否应替换为模块文案（按内容判定，不看指针）。
bool desc_should_replace(const char* text);

// 总开关（配置项 customRecipeEnabled）。两功能（原 gemcraft 的界面行为与本配方域）共用它。
bool set_enabled(bool enabled);
bool enabled();

// 注册「进档重建动态特殊装备配方」回调（幂等）。
void register_dynamic_recipes();

}  // namespace custom_recipe
