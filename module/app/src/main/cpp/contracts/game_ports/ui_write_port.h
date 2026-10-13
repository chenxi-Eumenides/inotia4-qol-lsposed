#pragma once

#include <cstdint>
#include <type_traits>

#include "../result.h"
#include "../version.h"

namespace qol::contracts {

// core.ui owner 的面板写端点类别。
constexpr int32_t kQolUiWritePanelOpen = 0;   // POST /api/ui/open_panel（panel）
constexpr int32_t kQolUiWritePanelClose = 1;  // POST /api/ui/close_panel
constexpr int32_t kQolUiWriteMainMenu = 2;    // POST /api/ui/go_main_menu（无参）

// 执行面板写并写出迁移前的结果 JSON（面板写成功 {"ok":true,"state":<gamestate JSON>}；
// go_main_menu 成功 {"ok":true,"state":<player JSON>}；失败原错误体）。必须在游戏主线程
// 调用（由 owner 结果处理器执行）。panel 仅 kQolUiWritePanelOpen 使用（可为空串）。
using QolUiWriteFn = QolResult (*)(int32_t kind, const char* panel, char* out, uint32_t capacity,
                                   uint32_t* out_size);

// 推进一次 owner UI 写操作。NotReady 表示当前帧尚未完成；调用方可将 owner 命令延迟到下一帧。
using QolUiWriteStepFn = QolResult (*)(uint64_t operation_id, uint32_t attempt, int32_t kind,
                                       const char* panel, char* out, uint32_t capacity,
                                       uint32_t* out_size);

// 取消指定操作的 Game Adapter 等待状态；operation_id=0 清除本 owner 的全部等待状态。
using QolUiWriteCancelPendingFn = QolResult (*)(uint64_t operation_id);

struct QolUiWritePortReadApi {
    QolUiWriteFn write;
    QolUiWriteStepFn step;
    QolUiWriteCancelPendingFn cancel_pending;
};

struct QolUiWritePort {
    QolVersion abi_version;
    QolUiWritePortReadApi read;
};

using QolUiStateReadSnapshotFn = QolResult (*)(char* screen, uint32_t screen_capacity,
                                                uint32_t* screen_size, char* panel,
                                                uint32_t panel_capacity, uint32_t* panel_size,
                                                char* dialog, uint32_t dialog_capacity,
                                                uint32_t* dialog_size);

struct QolUiStateReadApi {
    QolUiStateReadSnapshotFn read_snapshot;
};

struct QolUiStateReadPort {
    QolVersion abi_version;
    QolUiStateReadApi read;
};

static_assert(std::is_standard_layout<QolUiWritePort>::value,
              "QolUiWritePort must remain standard layout");
static_assert(std::is_trivially_copyable<QolUiWritePort>::value,
              "QolUiWritePort must remain trivially copyable");
static_assert(std::is_standard_layout<QolUiStateReadPort>::value,
              "QolUiStateReadPort must remain standard layout");
static_assert(std::is_trivially_copyable<QolUiStateReadPort>::value,
              "QolUiStateReadPort must remain trivially copyable");

}  // namespace qol::contracts
