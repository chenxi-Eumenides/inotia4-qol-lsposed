#pragma once

#include <cstdint>

#include "result.h"
#include "version.h"

// world_tuning 对外发布的移速能力：其他插件（例如移动优化）按需消费，
// 不需要知道倍率档位或配置格式。1 倍 = 8 逻辑单位，返回值已按整数取整。
//
// v1.1 追加订阅：提供方在倍率变化、启用/停用和订阅建立时主动推送当前步长，
// 消费端不需要轮询能力或在使用时重新读取。
namespace qol::contracts {

constexpr const char* kWorldTuningMoveSpeedCapabilityId = "world_tuning.move_speed";
constexpr QolVersion kWorldTuningMoveSpeedVersion{1, 1};
constexpr int32_t kWorldTuningBaseMoveSpeed = 8;

using WorldTuningMoveSpeedReadFn = QolResult (*)(int32_t* out_speed);
// ctx 由订阅方持有；speed 为当前步长（>=1）。回调在提供方主线程同步执行。
using WorldTuningMoveSpeedObserver = void (*)(void* ctx, int32_t speed);

struct WorldTuningMoveSpeedApi {
    // 写入当前移速（>=1）；失败时不改写 out。
    WorldTuningMoveSpeedReadFn read;
    // 订阅后立即回调一次当前值；重复订阅返回新 id，不覆盖旧订阅。
    QolResult (*subscribe)(void* ctx, WorldTuningMoveSpeedObserver observer,
                           uint64_t* out_id);
    QolResult (*unsubscribe)(uint64_t id);
};

}  // namespace qol::contracts
