#pragma once

#include <cstdint>

#include "../../contracts/plugin.h"
#include "../../contracts/character_facts.h"

// 简单模式插件业务层（ARCHITECTURE.md §3.3）。
//
// 功能：开启后
//   ① 怪物/敌人最大生命在生成与任何属性重算路径上减半；
//   ② 玩家侧对怪物造成的伤害 +50%（×1.5）；
//   ③ 玩家侧受到的伤害 −50%（×0.5）。
//
// 机制（经深反汇编论证后采用入口 hook 单点包裹，见 archive/docs/development/plugins/simple-mode.md）：
//   - 伤害倍率：包裹 CHAR_AddDamage 入口。该函数是全部 15 条伤害路径的唯一汇合点，函数体第 3 条
//     语句即 CHAR_AddLife(受害者, -伤害)，故入口即「生效前最后一刻」；只改 w2 一个入参，
//     扣血/承伤记录/仇恨/飘字/音效全部一致缩放。
//   - 最大生命：包裹 CHAR_UpdateAttrFromMonster。CHAR_UpdateAttr 对 C_TYPE==1 必然分派至此，
//     且属性脏位是惰性重算 → 生成/升级/装备/buff/难度 任何路径都会经过。
//
// 线程模型：门禁由配置事件（主线程）写，游戏线程读（原子）；两个 wrapper 只在游戏线程被调用，
// wrapper 内会调游戏查询函数（阵营判定）——这些函数均为纯读、无写回副作用，但不得在其它线程
// 调用本域任何入口。半血幂等账本仅游戏线程访问。
//
// 资源安装由宿主声明式执行（plugin.cpp 声明，host/plugin_registry 安装），原函数入口 backup
// 经 on_resource_ready 交还本层。

namespace qol::features::simple_mode {

const contracts::QolHostSdk* host_sdk();
const contracts::QolGameHost* game_host();
void set_character_facts(const contracts::CharacterFactsApi* facts);


// 资源 id：plugin.cpp 的 ResourceClaim 与本层 on_resource_ready 分派共用同一常量。
inline constexpr const char* kResourceIdAddDamage = "hook:CHAR_AddDamage";
inline constexpr const char* kResourceIdUpdateAttrFromMonster = "hook:CHAR_UpdateAttrFromMonster";

// 门禁开关：true = 业务生效；false = wrapper 空转透传（fail-closed）。任意线程可读。
void set_gate(bool enabled);
bool gate_enabled();

// 宿主在资源安装成功后交还 backup（原函数入口）；未知 resource_id 记一次告警后忽略。
// 受限生命周期回调（§3.3）：只缓存 backup，恒返回 0（本插件无失败路径）。
int32_t on_resource_ready(contracts::PluginContext& ctx, const char* resource_id, void* backup);

// 两个入口 wrapper：由 plugin_descriptor 的 ResourceClaim.handler 声明，宿主安装。
void char_add_damage_wrapper(void* attacker, void* victim, int32_t damage, int32_t damage_flag,
                             int32_t magic_flag);
void char_update_attr_from_monster_wrapper(void* ch, int32_t attr);

}  // namespace qol::features::simple_mode
