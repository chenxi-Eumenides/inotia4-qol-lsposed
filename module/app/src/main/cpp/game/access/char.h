#pragma once

#include <cstdint>

#include "data/native/game_symbols.h"

// 游戏层：角色对象语义化访问（ARCHITECTURE.md §5）。
// 偏移常量与 fn_* 游戏函数指针收口在本层；上层只调用语义函数，不出现裸偏移/裸地址。
//
// 线程纪律：以下函数只应在游戏线程调用（依赖角色池与队伍全局）。阵营判定三原语
// 均为纯读、无写回副作用，但同样不得在其它线程调用。

namespace qol::game {

// 角色池容量（半血幂等账本按池槽索引，数组尺寸需编译期常量）。
inline constexpr int kCharPoolSlots = C_CHARSYSTEM_POOL_SLOTS;

// attr_id 是否为最大生命槽（ATTR_MAX_HP）。
bool char_attr_is_max_hp(int32_t attr_id);

// 角色类型：0=玩家侧角色 1=怪物 2=NPC/装饰物；ch 为空返回 -1。
int char_type(const void* ch);

// 最大生命槽（C_MAX_HP）读写；ch/out 为空返回 false。
bool char_read_max_hp(const void* ch, int32_t* out);
bool char_write_max_hp(void* ch, int32_t value);

// 当前生命（C_HP）读写；ch/out 为空返回 false。
bool char_read_hp(const void* ch, int32_t* out);
bool char_write_hp(void* ch, int32_t value);

// 把角色指针换算为角色池槽号；返回 false = 不在池内/未对齐/基址未就绪。
bool char_pool_slot(const void* ch, int* out_slot);

// 阵营判定依赖的三个原语是否都已解析。
bool char_faction_deps_ready();
// 三原语各自的解析状态（诊断日志用）。
bool char_party_index_resolved();
bool char_active_group_resolved();
bool char_summoner_resolved();

// 队伍索引：CHAR_GetPartyIndex(ch) 原值；-1 = 不在队伍。ch 为空或原语未解析同样返回 -1。
int char_party_index(const void* ch);

// 主角/队友判定：CHAR_GetPartyIndex(ch) != -1。
bool char_is_party_member(void* ch);
// 当前主控或主控召唤物判定：CHAR_IsActivePlayerGroup(ch) != 0。
bool char_is_active_player_group(void* ch);
// 召唤者反查；无召唤者或依赖缺失返回 nullptr。
void* char_get_summoner(void* ch);

}  // namespace qol::game
