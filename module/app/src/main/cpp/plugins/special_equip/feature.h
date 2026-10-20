#pragma once

#include <cstdint>

#include "../../contracts/plugin.h"
#include "../../contracts/item_facts.h"
#include "../../contracts/character_facts.h"
#include "../../contracts/game_ports/character_extras_port.h"

// 特殊装备解锁（special-equip）插件业务层。
//
// 功能：解除 ITEMDATABASE 记录 +7 bit4 这道「NPC 专属保护」对**非特殊 NPC** 的约束，
// 使玩家侧角色能够穿脱全部带该位保护的装备。全表共 26 条（静态数据实证）：
//   cat 485-506  敌方/剧情专属组（镇魂之杖、冰焰戒指、龙齿剑、巨人族的长靴……）
//   cat 785-787  誓约之剑 / 神速长靴 / 真实的板甲
//   cat 948      伪装用面具
// 该位同时是「不可装备」与「不可脱下」的唯一开关（两个判定函数各读它一次）。
//
// 机制：包裹 `CHAR_CanEquipItem` 与 `CHAR_CanUnequipItem` 两个入口 hook（资源由宿主
// 声明式安装，见 plugin.cpp）；出售按钮 `UIEquip_ButtonDestroyExe` 已被 inventory 域 hook
// （同址重复 hook 返回 -1），经 hooks/dispatch 布尔门登记谓词挂入门禁（D2c-2b-1）。
// 原版判定链：
//   CanEquipItem:   ① ITEM_IsRealEquip(item) → ② !(rec[+7] & 0x10)
//                   → ③ CHAR_CanChangeEquip(ch) → ④ ch[C_LEVEL] >= ITEM_GetEquipLevel(item)
//   CanUnequipItem: ① !(rec[+7] & 0x10) → ② CHAR_CanChangeEquip(ch)
// 本域只在**原版已返回 0** 时跳过 bit4 那一项并重放其余检查；其余各道原样执行。
//
// 关键设计取舍：
//   - **不做「临时清位重跑」**。那需要写 ITEMDATABASE；wrapper 对游戏状态只做纯读。
//     item handle 的注册/查询要求主线程，非主线程进入会在 item_facts_view 失败并保持原版拒绝。
//   - **不 hook `CHAR_CanChangeEquip`**。它签名 `int(void* ch)`，拿不到 item，无法把放行限定到
//     具体装备；而且它体内**不读 bit4**（只读 `[ch+0x352]` 与 CHAR_IsSpecialNPC），hook 它
//     碰不到目标；更重要的是它是**必须保留**的那道检查。
//   - **不直接返回 1**。那会一并跳过 ①③④：非装备物品可被「穿上」、低等级可穿高等级装备，
//     且特殊 NPC 的锁（③）当场失效。
//
// 角色闸门完全交给原版 ③：`CHAR_CanChangeEquip(ch) = ([ch+0x352] >= 0) && !CHAR_IsSpecialNPC(ch)`。
// 于是「特殊 NPC 身上的无法穿脱」由原版逻辑保证——任务特殊 NPC 与不在队伍槽的角色（怪物、
// 场景 NPC）照旧被拒，本域不参与角色判定。
//
// 出售侧（装备详情菜单的出售按钮）：原版 UIEquip_ButtonDestroyExe → OKDestroyItem
// 装备分支无任何门禁（不查 IsNoSell/bit3——cat 785-787/948 本就无 bit3，也不查穿脱
// 判定与角色归属），特殊装备穿到 NPC 佣兵身上后可被原样卖出。注册到出售按钮 wrapper
// 的谓词在创建确认弹窗前校验菜单角色 CHAR_CanChangeEquip（与穿脱闸门同一角色语义），
// 特殊 NPC 佣兵身上装备拒绝出售；背包物品（desc_type==2）放行。
//
// 默认开启，作为 Hidden toggle 暴露：Debug 可经 feature 关闭并触发生命周期卸载，Release 不显示 Hidden。
//
// 线程模型：两个 wrapper 对游戏状态只做纯读；item handle 的注册/查询要求主线程，非主线程进入
// 会在 item_facts_view 失败并保持原版拒绝。

namespace qol::features::special_equip {

const contracts::QolHostSdk* host_sdk();
const contracts::QolGameHost* game_host();
const contracts::QolCharacterExtrasPort* character_extras();
void set_item_facts(const contracts::ItemFactsApi* facts);
void set_character_facts(const contracts::CharacterFactsApi* facts);

void log_debug(const char* format, ...);
void log_info(const char* format, ...);
void log_warn(const char* format, ...);
void log_error(const char* format, ...);

// 资源 id：plugin.cpp 的 ResourceClaim 与本层 on_resource_ready 分派共用同一常量。
inline constexpr const char* kResourceIdCanEquipItem = "hook:CHAR_CanEquipItem";
inline constexpr const char* kResourceIdCanUnequipItem = "hook:CHAR_CanUnequipItem";

// 业务门禁：true = 放行 bit4 保护；false = wrapper 空转透传原版（fail-closed）。任意线程可读。
void set_gate(bool enabled);
bool gate_enabled();

// 重放依赖是否齐备。缺任一即 fail-closed：install 返回非 0（宿主不安装资源、置 failed），
// wrapper 也不放行。依赖：ITEM_IsRealEquip / ITEM_GetEquipLevel / CHAR_CanChangeEquip /
// PARTY_GetMenuCharacter（两个入口 hook 的符号解析由宿主负责）。
bool deps_ready();

// deps_ready() 为假时的安装期诊断（逐项列出缺失的重放原语）。
void log_deps_missing();

// 宿主在资源安装成功后交还 backup（原函数入口）；未知 resource_id 记一次告警后忽略。
// 受限生命周期回调：只缓存 backup，恒返回 0。
int32_t on_resource_ready(contracts::PluginContext& ctx, const char* resource_id, void* backup);

// 两个入口 wrapper：由 plugin_descriptor 的 ResourceClaim.handler 声明，宿主安装。
int can_equip_wrapper(void* ch, void* item);
int can_unequip_wrapper(void* ch, void* item);

// 出售门禁谓词（D2c-2b-1）：经 hooks/dispatch 布尔门登记到 inventory_hooks 的
// UIEquip_ButtonDestroyExe wrapper；入口先判业务门禁。ctx 未使用（保留签名）。
int32_t sell_gate_should_block(void* ctx);

}  // namespace qol::features::special_equip
