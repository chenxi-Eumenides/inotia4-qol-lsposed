#pragma once

#include <cstdint>

namespace qol::game {

// 存档写结果形态：kOk 已结算成功；kFailed 已结算失败（out 携带原错误体）；
// kNotReady 尚未就绪（转场后世界态未提交），调用方应保持票据 pending 并下一帧重试。
constexpr int32_t kSaveWriteOk = 0;
constexpr int32_t kSaveWriteFailed = 1;
constexpr int32_t kSaveWriteNotReady = 2;

// save_backup owner 的存档写结果拼装接缝（实现在 legacy 聚合 TU game_system.cpp，该文件可访问
// api/native 与 infra/store；game 层适配端口只依赖本声明）。
//
// 必须在游戏主线程调用，由 save_backup 的结果处理器执行；HTTP handler 不直接调用。
// kind 取 contracts::kQolSaveWrite*；attempt 为命令执行次数（首次 0）：首次发起游戏侧操作，
// 后续帧仅等待世界态就绪后结算（含 sidecar 槽位生命周期）。
int32_t build_save_write_result_json(int32_t kind, int32_t slot, int32_t class_idx, uint32_t attempt,
                                     char* out, uint32_t capacity, uint32_t* out_size);

}  // namespace qol::game
