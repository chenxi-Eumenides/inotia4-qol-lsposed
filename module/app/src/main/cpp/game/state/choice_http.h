#pragma once

#include <cstdint>

namespace qol::game {

// 对话写结果形态：kOk 已结算成功；kFailed 已结算失败（out 携带原错误体）。
constexpr int32_t kChoiceWriteOk = 0;
constexpr int32_t kChoiceWriteFailed = 1;

// core.choice owner 的对话写结果拼装接缝（实现在 legacy 聚合 TU game_system.cpp，该文件可访问
// api/native；game 层适配端口只依赖本声明）。
//
// 必须在游戏主线程调用，由 core.choice 的结果处理器执行；HTTP handler 不直接调用。
// kind 取 contracts::kQolChoiceWrite*；迁移前 dialog/select 与 start_interact 均原样透传 op JSON。
int32_t build_choice_write_result_json(int32_t kind, const char* action, int32_t index, char* out,
                                       uint32_t capacity, uint32_t* out_size);

}  // namespace qol::game
