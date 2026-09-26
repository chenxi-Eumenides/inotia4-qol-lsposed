// game_world.cpp —— 世界域：剧情状态 + 自研 BFS 路径/距离/调试路径（parse 域）
// 由 game_misc.cpp 拆分生成（纯搬代码，零逻辑变更）。

#include "game_world.h"

#include "game/access/game_access.h"
#include "game/json/game_json.h"
#include "game/nav/game_nav.h"
#include "game/state/game_state.h"
#include "game/ops/game_ops_common.h"
#include "game/read/game_tiles.h"

#include <cstdint>
#include <vector>

#include "game_world_story.inc"
#include "game_world_navigation.inc"
#include "game_world_readers.inc"
