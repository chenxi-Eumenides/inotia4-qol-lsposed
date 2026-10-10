#pragma once

#include "../../contracts/craft_result.h"
#include "../../contracts/craft_commands.h"
#include "../../contracts/craft_facts.h"
#include "../../contracts/game_host.h"
#include "../../contracts/game_ports/custom_recipe_write_port.h"
#include "../../contracts/host_sdk.h"
#include "../../contracts/ownership.h"
#include "../../contracts/item_facts.h"

namespace qol::contracts {
struct PluginContext;
}

// 自定义合成配方结果层 feature（C2 Step 4）：结果判定/扣料/产物与 RECIPEBASE/MIXTUREBASE
// 表注入所有权的实现。
//
// 边界（「界面归界面，结果归结果」，ADR-0005）：
//   · provider 不读 UIMix/控件：界面状态（选中物品、三格内容、持有数）由调用方经
//     contracts/craft_result.h 的 Input 结构体携带；provider 只做判定与结果计算。
//   · 扣料/产物/入包与 stuffList 需求写回属「写入执行」，同步留在本 feature。

namespace qol::features::custom_recipe {

void set_context(const contracts::PluginContext& ctx);
void set_item_facts(const contracts::ItemFactsApi* facts);
void set_craft_facts(const contracts::CraftFactsApi* facts);
void set_write_port(const contracts::QolCustomRecipeWritePort* port);
const contracts::QolHostSdk* host_sdk();
const contracts::QolGameHost* game_host();
void log_message(int32_t level, const char* format, ...);

// CraftResultApi 实例（provides `craft_result` v1.1）。
const qol::contracts::CraftResultApi& craft_result_api();

// RECIPEBASE/MIXTUREBASE 表注入的行为所有权 ops。
const qol::contracts::OwnershipOps& ownership_ops();

// 门禁：镜像 descriptor feature `custom_recipe.recipes` 的运行态；由生命周期与
// feature on_toggle 驱动。
bool enabled();
bool set_enabled(bool enabled);

// 注册「进档重建动态特殊装备配方」回调（幂等）。
void register_dynamic_recipes();
void unregister_dynamic_recipes();

bool handle_write_stuff_need_command(const uint8_t* payload, uint32_t size);
bool handle_set_stuff_item_command(const uint8_t* payload, uint32_t size);

}  // namespace qol::features::custom_recipe
