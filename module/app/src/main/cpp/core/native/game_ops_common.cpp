// game_ops_common.cpp —— op_ok/op_err 响应信封（自 game_state.cpp 迁入，纯搬代码，零逻辑变更）

#include "game_ops_common.h"

#include "game_cache.h"

#include <string>

std::string op_ok() {
    frame_cache_force_refresh();   // 写操作成功后同步刷新缓存（attach 立即读最新，v0.4.57）
    return "{\"ok\":true}";
}

std::string op_err(const char* msg) {
    return std::string("{\"ok\":false,\"error\":\"") + msg + "\"}";
}
