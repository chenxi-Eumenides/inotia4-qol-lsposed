#pragma once

#include <cstdint>

#include "rates.h"
#include "../../contracts/plugin.h"
#include "../../contracts/character_facts.h"
#include "../../contracts/game_ports/character_extras_port.h"
#include "../../contracts/item_use.h"

// 游戏世界调整插件业务层（ARCHITECTURE.md §3.3）。
//
// 功能：按插件配置中的倍率缩放世界数值
//   ① 敌怪最大生命；
//   ② 玩家侧对怪物造成的伤害；
//   ③ 玩家侧受到的伤害（面板「承伤倍率」）；
//   ④ 玩家侧获得的经验；
//   ⑤ 金币增量。
// 掉落数量与重抽分别控制掉落结算次数和槽内造物重掷次数。
//
// 机制（沿用 simple_mode 已论证的入口 hook）：
//   - 伤害：包裹 CHAR_AddDamage 入口。该函数是全部 15 条伤害路径的唯一汇合点，
//     函数体第 3 条语句即 CHAR_AddLife(受害者, -伤害)，故入口即「生效前最后一刻」。
//   - 最大生命：包裹 CHAR_UpdateAttrFromMonster。CHAR_UpdateAttr 对 C_TYPE==1 必然分派至此，
//     属性脏位是惰性重算 → 生成/升级/装备/buff/难度 任何路径都会经过。
//
// 线程模型：倍率由主线程写入原子变量，游戏线程 wrapper 读取；两个 wrapper 只在游戏线程执行。
// 幂等账本（raw/applied）仅游戏线程访问。
//
// 资源安装由宿主声明式执行（plugin.cpp 声明，host/plugin_registry 安装），原函数入口 backup
// 经 on_resource_ready 交还本层。

namespace qol::features::world_tuning {

const contracts::QolHostSdk* host_sdk();
const contracts::QolGameHost* game_host();
const contracts::QolCharacterExtrasPort* character_extras();
const contracts::CharacterFactsApi* character_facts();

// 插件生命周期期写入（host 主线程）。安装失败路径会清空。
void bind_runtime(const contracts::QolHostSdk* sdk, const contracts::QolGameHost* game,
                  const contracts::QolCharacterExtrasPort* extras,
                  const contracts::CharacterFactsApi* facts,
                  contracts::QolOwnerToken owner, const char* plugin_id);
void clear_runtime();

// 资源 id：plugin.cpp 的 ResourceClaim 与本层 on_resource_ready 分派共用同一常量。
inline constexpr const char* kResourceIdAddDamage = "hook:CHAR_AddDamage";
inline constexpr const char* kResourceIdUpdateAttrFromMonster = "hook:CHAR_UpdateAttrFromMonster";
inline constexpr const char* kResourceIdAddExperience = "hook:CHAR_AddExperience";
inline constexpr const char* kResourceIdAddMoney = "hook:INVEN_AddMoney";
inline constexpr const char* kResourceIdDropItem = "hook:CHARSYSTEM_DropItem";
inline constexpr const char* kResourceIdMakeItem = "hook:ITEMSYSTEM_MakeItem";
inline constexpr const char* kResourceIdMakeEquipDetail = "hook:ITEMSYSTEM_MakeEquipDetail";
inline constexpr const char* kResourceIdGetActionCoolTime = "hook:CHAR_GetActionCoolTime";

// 门禁开关：true = 业务生效；false = wrapper 空转透传（fail-closed）。任意线程可读。
void set_gate(bool enabled);
bool gate_enabled();

// 倍率快照与应用。apply 可从主线程（面板/配置）调用；wrapper 侧只读原子值。
Rates current_rates();
void apply_rates(const Rates& rates);
// 从本插件 config 的 "rates" 键读取并应用；成功返回 true。缺失/不可用返回 false 并保持现值。
bool reload_rates_from_config();
// 通过 owner 管理面把 rates 写入持久化配置；成功受理返回 true 并回填 request_id。
bool request_persist_rates(const Rates& rates, uint64_t* out_request_id);
// 面板轮询：读取持久化 receipt。配置管理面不可用返回 false。
bool poll_rates_receipt(uint64_t request_id, contracts::FeatureReceipt* out);
// 配置管理面（QolFeatureApi v1.30）是否可用；面板据此禁用箭头。
bool rates_config_available();

// 宿主在资源安装成功后交还 backup（原函数入口）；未知 resource_id 记一次告警后忽略。
// 受限生命周期回调（§3.3）：只缓存 backup，恒返回 0（本插件无失败路径）。
int32_t on_resource_ready(contracts::PluginContext& ctx, const char* resource_id, void* backup);

// 两个入口 wrapper：由 plugin_descriptor 的 ResourceClaim.handler 声明，宿主安装。
void char_add_damage_wrapper(void* attacker, void* victim, int32_t damage, int32_t damage_flag,
                             int32_t magic_flag);
void char_update_attr_from_monster_wrapper(void* ch, int32_t attr);
int32_t char_add_experience_wrapper(void* ch, int32_t amount, uint8_t flag);
int32_t inven_add_money_wrapper(int64_t amount);
void drop_item_wrapper(void* ch);
void* make_item_wrapper(int32_t category, int32_t lookup_key, int32_t flag);
void* make_equip_detail_wrapper(int32_t kind, int32_t min_level, int32_t max_level,
                                int32_t item, int32_t option);

// 技能冷却倍率：CHAR_GetActionCoolTime 入口 wrapper。
int32_t char_get_action_cooltime_wrapper(void* ch, void* action);

// item.use 后置通知：绑定/解绑（provider 停用时解绑）。
void bind_item_use(const contracts::ItemUseApi* api);
void unbind_item_use();

}  // namespace qol::features::world_tuning
