#pragma once
#include "contracts/save_file_evidence.h"
namespace qol::game {
// 只读取调用方明确指定的原始 dat，不搜索目录、不写文件、不用 mtime 或明文替代。
bool read_save_file_evidence(const char* dat_path, contracts::QolSaveFileEvidence* out);
}  // namespace qol::game
