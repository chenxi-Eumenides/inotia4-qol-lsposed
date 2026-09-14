#pragma once

namespace world_teleport {

constexpr int kMaxMapId = 414;

// 计算传送选项目标地图 ID。地图 ID 在 [0, kMaxMapId] 内循环。
int target_map_id(int current_id, int delta);

}  // namespace world_teleport
