#pragma once

#include <cstdint>

// 推怪纯逻辑层（不触内存、不调游戏函数、无平台依赖）：
// 只做「CHAR_GetPartyIndex 替代值」的决定，供 host test 穷举矩阵；
// 角色类型与阵营查询本身在 feature.cpp（需读游戏内存与调游戏函数）。

namespace qol::features::push_all {

// 推动分支队伍闸门的返回值（CHAR_GetPartyIndex 的替代）：
//   type==1（怪物）            → 0    （模块新增：伪装为队伍成员使其可推）
//   is_active_player_group      → 0    （主控本人/主控召唤物）
//   其余                        → party_index（队友 >=0 可推；场景设施 -1 不可推）
int party_decision(uint8_t type, bool is_active_player_group, int party_index);

}  // namespace qol::features::push_all
