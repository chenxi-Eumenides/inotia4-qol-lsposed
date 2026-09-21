// game_world.cpp —— 世界域：剧情状态 + 自研 BFS 路径/距离/调试路径（parse 域）
// 由 game_misc.cpp 拆分生成（纯搬代码，零逻辑变更）。

#include "game_world.h"

#include "game_access.h"
#include "game/json/game_json.h"
#include "game/nav/game_nav.h"
#include "game_state.h"
#include "game/ops/game_ops_common.h"
#include "infra/frame/frame_task.h"
#include "infra/log/qol_log.h"
#include "infra/frame/transition_dispatch.h"
#include "game/read/game_tiles.h"

#include <cstdint>
#include <vector>

#include "game_world_story.inc"
#include "game_world_navigation.inc"
#include "game_world_movement.inc"
#include "game_world_operations.inc"
#include "game_world_readers.inc"
