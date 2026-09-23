#pragma once

#include <cstdint>

namespace qol::game {

// 世界写结果形态：kOk 已结算成功；kFailed 已结算失败（out 携带原错误体）。
constexpr int32_t kWorldWriteOk = 0;
constexpr int32_t kWorldWriteFailed = 1;

// world_teleport owner 的世界写结果拼装接缝（实现在 legacy 聚合 TU game_system.cpp，该文件可访问
// api/native；game 层适配端口只依赖本声明）。
//
// 必须在游戏主线程调用，由 world_teleport 的结果处理器执行；HTTP handler 不直接调用。
// kind 取 contracts::kQolWorldWrite*；失败时 out 携带原错误体。
int32_t build_world_write_result_json(int32_t kind, int32_t a, int32_t b, int32_t c, int32_t d,
                                      char* out, uint32_t capacity, uint32_t* out_size);

}  // namespace qol::game
