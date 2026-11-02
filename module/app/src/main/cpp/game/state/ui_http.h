#pragma once

#include <cstdint>
#include <string>

#include "contracts/result.h"

namespace qol::game {

// core.ui owner 的面板写结果状态（供适配端口映射票据失败）。
enum : int32_t {
    kUiWriteOk = 0,
    kUiWriteFailed = 1,
};

// POST /api/ui/{open_panel,close_panel}：执行面板写并把迁移前的结果 JSON 写入 out
//（成功 {"ok":true,"state":<gamestate JSON>}，失败原错误体）。必须在游戏主线程调用。
int32_t build_ui_write_result_json(int32_t kind, const char* panel, char* out, uint32_t capacity,
                                  uint32_t* out_size);

// Returns the post-write player or gamestate snapshot through the existing UI owner seam.
QolResult ui_write_result_state_json(int32_t kind, std::string* out_json);

}  // namespace qol::game
