#pragma once

#include <cstdint>

#include "../result.h"
#include "../version.h"

namespace qol::contracts {

// character_facts owner 读取的响应体类别。body_json 由游戏主线程的帧任务调用，
// 结果发布为插件私有快照后 HTTP handler 只读；HTTP 线程不直接调用。
enum class QolCharacterHttpJsonKind : int32_t {
    kPlayer = 0,        // 原生 player JSON（party/count 与 leader_slot 用）
    kParty = 1,         // 原生 party 数组 + 迁移前 Kotlin 成员富化/字段重排
    kSkills = 2,        // 原生 skills 数组 + 迁移前 Kotlin 技能名注入
    kMercenaries = 3,   // 原生 mercenaries JSON（原样透传）
};

using QolCharacterHttpJsonFn = QolResult (*)(int32_t kind, char* out, uint32_t capacity,
                                             uint32_t* out_size);

struct QolCharacterHttpPortReadApi {
    QolCharacterHttpJsonFn body_json;
};

struct QolCharacterHttpPort {
    QolVersion abi_version;
    QolCharacterHttpPortReadApi read;
};

}  // namespace qol::contracts
