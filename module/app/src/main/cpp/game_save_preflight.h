#pragma once

#include <cstdint>
#include <cstddef>
#include <string>

// 存档预检（parse 域，纯逻辑）：槽结构状态字节 → 完整性判决。
// 判决依据：docs/system/save.md §4/§7（原版 SAVE_LoadSaveSlot 加载判决 b2/b3，
// 与 UI「存档损坏」严格一致）。本文件零游戏依赖（不 include game_access/game_symbols），
// 可直接编入 host 单测（tests/CMakeLists.txt）。

// 判决五态（backlog P0②）
enum class SavePreflightVerdict { kValid, kCorrupt, kMissing, kIncompatible, kUnknown };

// 槽状态字节（slot+2）：0=缺失 1=加载失败 2=加载成功；失败码取 slot+3 bits[2:0]（bits[5:3] 为槽位号，需掩除）。
SavePreflightVerdict save_preflight_classify(uint8_t slot_state, uint8_t slot_err);

const char* save_preflight_verdict_name(SavePreflightVerdict v);

// 失败阶段名（docs/system/save.md §4 阶段码表；>9 返回 "unknown"）
const char* save_preflight_stage_name(uint8_t err_code);

// enter_slot 内部使用的结构化完整性判定结果。
// detail 非空时附加 "detail" 字段（如 "not in main menu (state=5)"）。
std::string save_preflight_json(int32_t slot, uint8_t slot_state, uint8_t slot_err,
                                int map_id, int hero_level, int hero_index,
                                const char* detail = nullptr);

// enter_slot 拒绝体：错误信封（格式 A {"ok":false,"error":...}）+ 机器可读字段。
std::string save_preflight_error_json(int32_t slot, uint8_t slot_state, uint8_t slot_err);

std::string save_preflight_semantic_error_json(int32_t slot, const char* stage,
                                               uint8_t error_code, const char* detail = nullptr);
