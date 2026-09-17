#pragma once

#include "contracts/plugin.h"

// 自定义合成配方插件清单入口（ARCHITECTURE.md §3.3；C2 Step 4）。
//
// provides `craft_result` v1.0（结果层能力，消费者=features/craft_ui）；optional_requires
// `attr_range`（宝石档位判定，缺失只 degraded、不级联）；声明 RECIPEBASE/MIXTUREBASE 表注入的
// kOwnership 资源（gate=customRecipeEnabled，acquire=table_ensure / release=table_deactivate）。

namespace qol::features::custom_recipe {

const contracts::PluginDescriptor& plugin_descriptor();

}  // namespace qol::features::custom_recipe
