#pragma once

#include <cstdint>
#include <string>

#include "contracts/game_ports/character_write_port.h"

namespace qol::game {

// character **写面**的响应体拼装接缝（实现在 legacy 聚合 TU game_system.cpp）。
// 注意与同目录 character_http.h 区分：后者是角色**读面**拼装（party/skills/mercenaries）。
//
// 必须在游戏主线程调用；core.character 的结果处理器调用，HTTP handler 只读票据。
//
// 返回码：Ok = 成功（out 为迁移前原响应 JSON）；Failed = 业务失败（out 为原错误体）。

constexpr int32_t kCharacterWriteOk = 0;
constexpr int32_t kCharacterWriteFailed = 1;

// 按 args.kind 执行既有角色/战斗/成长/队伍写原语，并按迁移前 Kotlin 的形状组装响应：
//   数值 op（hp/mp/level/experience/set_attr/status-point）与 party swap → 原始 op JSON
//   战斗 cast/attack_target/set_auto_attack、队伍 include/exclude/discharge/withdraw
//     → {"ok":true,"state":<party JSON>}
//   战斗 switch_player/stop_combat、成长 reset_stat/reset_skill → {"ok":true,"state":<player JSON>}
//   成长 add_skill → {"ok":true,"state":<skills JSON>}
//   成长 add_stat → 单点 +1（批量与自定义 {"ok":true,"applied":[...]} 由调用方循环组装）
int32_t build_character_write_result_json(const contracts::QolCharacterWriteArgs& args, char* out,
                                          uint32_t capacity, uint32_t* out_size);

}  // namespace qol::game
