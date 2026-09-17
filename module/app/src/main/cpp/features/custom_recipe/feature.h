#pragma once

#include "contracts/craft_result.h"
#include "contracts/ownership.h"

// 自定义合成配方结果层 feature（C2 Step 4）：结果判定/扣料/产物与 RECIPEBASE/MIXTUREBASE
// 表注入所有权的实现。
//
// 边界（「界面归界面，结果归结果」，ADR-0005）：
//   · provider 不读 UIMix/控件：界面状态（选中物品、三格内容、持有数）由调用方经
//     contracts/craft_result.h 的 Input 结构体携带；provider 只做判定与结果计算。
//   · 扣料/产物/入包与 stuffList 需求写回属「写入执行」，同步留在本 feature。

namespace qol::features::custom_recipe {

// CraftResultApi 实例（provides `craft_result` v1.0）。
const qol::contracts::CraftResultApi& craft_result_api();

// RECIPEBASE/MIXTUREBASE 表注入的行为所有权 ops。
const qol::contracts::OwnershipOps& ownership_ops();

// 门禁（配置 customRecipeEnabled 的镜像；由插件配置事件与生命周期驱动）。
bool enabled();
bool set_enabled(bool enabled);

// 注册「进档重建动态特殊装备配方」回调（幂等）。
void register_dynamic_recipes();

}  // namespace qol::features::custom_recipe
