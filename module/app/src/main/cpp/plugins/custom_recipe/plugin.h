#pragma once

#include "../../contracts/plugin.h"

// 自定义合成配方插件清单入口（ARCHITECTURE.md §3.3；C2 Step 4）。
//
// 结果 API 由本插件内部 UI 子层调用，不对外提供 capability；optional_requires
// `attribute.quality`（宝石档位判定，缺失只 degraded、不级联）；声明 RECIPEBASE/MIXTUREBASE 表注入的
// kOwnership 资源（gate=nullptr，资源跟随生命周期；acquire=table_ensure / release=table_deactivate）。

namespace qol::features::custom_recipe {

const contracts::PluginDescriptor& plugin_descriptor();

}  // namespace qol::features::custom_recipe
