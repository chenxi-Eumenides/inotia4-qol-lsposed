#pragma once

#include <cstdint>

namespace world_teleport {

// Game Host ABI 尚未提供共同静态地图上限；运行时 record_count 可用时优先采用它。
constexpr int kMaxMapId = 414;

// 从运行时 MAPINFOBASE_nRecordCount 推导本次传送允许的最大地图 ID。
// count<=0 表示运行时字段不可用，回退到静态上限；调用方仍需记录该情况。
int max_map_id_from_record_count(int record_count);

// 计算传送选项目标地图 ID。地图 ID 在 [0, max_map_id] 内循环。
int target_map_id(int current_id, int delta, int max_map_id);

// 保留静态上限版本，供不具备运行时游戏上下文的 Host 规则测试使用。
int target_map_id(int current_id, int delta);

// 中点锚选点（c18 进图选点同口径）：64×64 通行矩阵，bit3=不可通行。
// 中点 → X 正/负、Y 正/负（步长 ≤32）→ 全图最近（切比雪夫）非阻挡格。
// 返回 false = 矩阵不足/全图无可站立格（调用方按数据错误处理，不得回退 (0,0) 照常传送）。
bool pick_landing(const uint8_t* tiles, int width, int height, int* out_x, int* out_y);

}  // namespace world_teleport
