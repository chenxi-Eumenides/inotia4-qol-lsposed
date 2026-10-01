#pragma once

#include <cstdint>

#include "../result.h"
#include "../version.h"

namespace qol::contracts {

// core.system owner 读取的响应体类别。body_json 由游戏主线程的帧任务调用，
// 结果发布为插件私有快照后 HTTP handler 只读；HTTP 线程不直接调用。
enum class QolSystemJsonKind : int32_t {
    kSnapshot = 0,  // GET /api/system/snapshot（原生快照 + 迁移前 Kotlin 重排/名称注入）
    kGame = 1,      // GET /api/system/game（原生快照 + info）
    kInfo = 2,      // GET /api/system/info（version/package 由启动期下发）
    kFrame = 3,     // GET /api/system/game_frame
};

using QolSystemEventsJsonFn = QolResult (*)(char* out, uint32_t capacity,
                                            uint32_t* out_size);

using QolSystemJsonFn = QolResult (*)(int32_t kind, char* out, uint32_t capacity,
                                      uint32_t* out_size);

struct QolSystemPortReadApi {
    QolSystemJsonFn body_json;
    QolSystemEventsJsonFn events_json;
};

struct QolSystemPort {
    QolVersion abi_version;
    QolSystemPortReadApi read;
};

}  // namespace qol::contracts
