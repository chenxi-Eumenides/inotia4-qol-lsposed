#pragma once

#include <cstdint>

#include "../../contracts/plugin.h"

// 推怪（推动增强）业务层（ARCHITECTURE.md §3.3）。
//
// 两个调用点 patch 协同放开 CHAR_Move 推动分支的 blocker 资格限制：
//   ① 队伍闸门（CHAR_Move+0x168）：BL 重定向到 push_all_party_filter，怪物/主控召唤物
//      视为队伍成员使其可推；队友沿用原索引；场景设施保持 -1 不可推。
//   ② 站立闸门（CHAR_Move+0x180）：条件分支置 NOP，非站立单位（战斗中怪物）也可被推。
// call patch 不支持空转（§4.2 规则 6）：门禁关闭即物理还原，wrapper 只在已安装期间被调用。
//
// 线程模型：门禁由配置事件（主线程）写、游戏线程读（原子）；wrapper 在游戏主线程
// CHAR_Move 调用链内执行，只读 C_TYPE + 调 game/access 的纯读查询，无线程安全问题。

namespace qol::features::push_all {

const contracts::QolHostSdk* host_sdk();
const contracts::QolGameHost* game_host();

// 资源 id：plugin.cpp 的 ResourceClaim 与宿主诊断共用同一常量。
inline constexpr const char* kResourceIdParty = "patch:CHAR_Move+0x168";
inline constexpr const char* kResourceIdStand = "patch:CHAR_Move+0x180";

// 门禁开关：true = 模块语义生效；false = 按原版索引透传。任意线程可读。
void set_gate(bool enabled);
bool gate_enabled();

// 宿主在资源安装成功后交还 backup（调用点 patch 无 backup，恒为 nullptr）；
// 受限生命周期回调（§3.3）：只允许常数时间操作，恒返回 0。
int32_t on_resource_ready(contracts::PluginContext& ctx, const char* resource_id, void* backup);

// 队伍闸门 wrapper：由 plugin_descriptor 的 ResourceClaim.handler 声明，宿主安装。
int push_all_party_filter(void* ch);

}  // namespace qol::features::push_all
