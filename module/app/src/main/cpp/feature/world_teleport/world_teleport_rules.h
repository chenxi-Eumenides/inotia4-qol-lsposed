#pragma once

#include "game_symbols.h"

namespace world_teleport {

constexpr int kMaxMapId = MAPINFOBASE_STATIC_MAX_MAP_ID;

// 从运行时 MAPINFOBASE_nRecordCount 推导本次传送允许的最大地图 ID。
// count<=0 表示运行时字段不可用，回退到静态上限；调用方仍需记录该情况。
int max_map_id_from_record_count(int record_count);

// 计算传送选项目标地图 ID。地图 ID 在 [0, max_map_id] 内循环。
int target_map_id(int current_id, int delta, int max_map_id);

// 保留静态上限版本，供不具备运行时游戏上下文的 Host 规则测试使用。
int target_map_id(int current_id, int delta);

}  // namespace world_teleport
