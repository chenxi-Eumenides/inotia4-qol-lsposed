#pragma once

#include <cstdint>

#include "../result.h"
#include "../version.h"

namespace qol::contracts {

// 骰子确认后的应用结果（POD，无 JSON）：base = 应用前基础属性，applied = 掷骰待确认结果，
// delta = applied - base。写后响应 JSON 由 core.item owner 组装，低层不持有 JSON/kind 语义。
constexpr uint32_t kQolItemDiceStatCount = 5;
struct QolItemDiceAcceptResult {
    int32_t base[kQolItemDiceStatCount]{};
    int32_t applied[kQolItemDiceStatCount]{};
    int32_t delta[kQolItemDiceStatCount]{};
};

using QolItemDiceRejectFn = QolResult (*)();
using QolItemDiceAcceptFn = QolResult (*)(QolItemDiceAcceptResult* out);

struct QolItemDiceWritePort {
    QolVersion abi_version;
    QolItemDiceRejectFn reject;
    // v1.1 尾部追加：确认并应用待处理骰子结果（reject 保留原名/签名）。
    QolItemDiceAcceptFn accept;
};

constexpr QolVersion kQolPortItemDiceWriteVersion{1, 1};

}  // namespace qol::contracts
