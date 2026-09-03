#pragma once

#include <string>

// 写操作共享工具（parse 引擎）：op_ok/op_err 响应信封。
// 原在 game_state（data 层）——op_ok 内 frame_cache_force_refresh 使 state 倒挂 cache 成环，
// 迁出至此合法化（parse 引擎 → parse 引擎 cache 同层，无环）。

std::string op_ok();
std::string op_err(const char* msg);
