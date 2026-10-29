#pragma once

#include <cstdint>

#include "result.h"
#include "version.h"

// world_tuning 对外发布的移速能力：其他插件（例如全向移动）按需消费，
// 不需要知道倍率档位或配置格式。1 倍 = 8 逻辑单位，返回值已按整数取整。
namespace qol::contracts {

constexpr const char* kWorldTuningMoveSpeedCapabilityId = "world_tuning.move_speed";
constexpr QolVersion kWorldTuningMoveSpeedVersion{1, 0};
constexpr int32_t kWorldTuningBaseMoveSpeed = 8;

using WorldTuningMoveSpeedReadFn = QolResult (*)(int32_t* out_speed);

struct WorldTuningMoveSpeedApi {
    // 写入当前移速（>=1）；失败时不改写 out。
    WorldTuningMoveSpeedReadFn read;
};

}  // namespace qol::contracts
