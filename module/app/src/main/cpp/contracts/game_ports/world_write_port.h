#pragma once

#include <cstdint>
#include <type_traits>

#include "../result.h"
#include "../version.h"

namespace qol::contracts {

// world_teleport owner 的世界写端点类别（参数经通用 a/b/c/d 传递）。
constexpr int32_t kQolWorldWriteMoveTo = 0;    // POST /api/world/movement/move_to（a=x, b=y）
constexpr int32_t kQolWorldWriteWalkDir = 1;   // POST /api/world/movement/walk_dir（a=direction）
constexpr int32_t kQolWorldWriteStopMove = 2;  // POST /api/world/movement/stop_move
constexpr int32_t kQolWorldWriteInteract = 3;  // POST /api/world/movement/interact_with
constexpr int32_t kQolWorldWriteTeleport = 4;  // POST /api/op/movement/teleport（a=mapId, b=x, c=y, d=force）

// 执行世界写并写出迁移前的结果 JSON。必须在游戏主线程调用（由 owner 结果处理器执行）。
// move_to/walk_dir/interact_with 迁移前经 Kotlin `ActionSupport.attachPlayer` 追加 state，
// 故成功时输出 {"ok":true,"state":<player>}；stop_move 与 teleport 输出原始 op JSON。
// 失败时 out 携带原错误体（{"ok":false,...}）。
using QolWorldWriteFn = QolResult (*)(int32_t kind, int32_t a, int32_t b, int32_t c, int32_t d,
                                      char* out, uint32_t capacity, uint32_t* out_size);

struct QolWorldWritePortReadApi {
    QolWorldWriteFn write;
};

struct QolWorldWritePort {
    QolVersion abi_version;
    QolWorldWritePortReadApi read;
};

static_assert(std::is_standard_layout<QolWorldWritePort>::value,
              "QolWorldWritePort must remain standard layout");
static_assert(std::is_trivially_copyable<QolWorldWritePort>::value,
              "QolWorldWritePort must remain trivially copyable");

}  // namespace qol::contracts
