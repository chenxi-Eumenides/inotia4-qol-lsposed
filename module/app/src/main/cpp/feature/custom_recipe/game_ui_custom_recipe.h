#pragma once

#include <cstdint>

// 合成器自定义配方（custom-craft-recipe §4.4/§4.5/§4.6/§4.11/§4.12）：
// 六处函数级 hook —— 放料改写 / 合成按钮前置校验 / 产物改写 / 菜单页签（确保注入）/
// UIMix_ButtonRecipeExe（配方点击按 Form 分派）/ UIMix_MakeDesc（模块记录错误描述抑制）——
// 与总开关、安装入口。
//
// 两条模块配方挂宝石强化页（type 1，group 3）：「合成」(kThreeSlotCraft) 排在前，
// 「宝石强化」(kJewelTierUp) 排在后。
//  · kThreeSlotCraft（§4.12）：放料与合成两条路径**都不转调原函数**，由本文件自实现
//    （任意物品可填入 + 按 3 格内容查表 + 原生 YesNo 回调产出/扣料）。
//  · kJewelTierUp：放料/合成/产物三个 hook 按 kind 介入（借 type 3 形态）。
//
// hook 全部装在原函数入口，按 mixType/kind 门控；不改 gemcraft 占用的 4 个 GOT 槽。
// 安装时机：nativeInit bridge_init 后调用（幂等，可后续重试）；启用时亦触发安装尝试。

// 设置总开关（JNI 入口调用）。启用即触发安装尝试与表注入；bridge 未就绪时 QOL_LOG_WARN 延迟。
bool set_custom_recipe_enabled(bool enabled);

// 当前总开关状态。
bool custom_recipe_enabled();

// 当前所选配方是否为「3 格隐式配方」模式（供其他 feature 判断是否让路）。
// 读 `[g_uimix + UIMIX_SLOT_MIXTYPE]`（+0x48）→ 目录 `def_for_mix_type` → `kind ==
// kThreeSlotCraft`；`g_uimix == nullptr` 时返回 false。
bool custom_recipe_three_slot_mode_active();

// 六处 hook 安装（幂等、bridge_ready 门控）。成功安装返回 true。
bool custom_recipe_ui_install_if_ready();
