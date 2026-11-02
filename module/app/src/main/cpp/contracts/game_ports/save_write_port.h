#pragma once

#include <cstdint>
#include <type_traits>

#include "../result.h"
#include "../version.h"

namespace qol::contracts {

// core.save owner 的存档写端点类别。
constexpr int32_t kQolSaveWriteSave = 0;       // POST /api/system/save
constexpr int32_t kQolSaveWriteEnterSlot = 1;  // POST /api/system/enter_slot
constexpr int32_t kQolSaveWriteCreateSlot = 2; // POST /api/system/create_slot

// 执行存档写并写出迁移前的结果 JSON（{"ok":true,"state":<player>} 或原错误体）。
// 必须在游戏主线程调用（由 owner 结果处理器执行）；attempt 为命令执行次数（首次 0），
// 用于区分「首次发起转换」与「后续帧等待世界态就绪」。就绪前返回 kQolErrNotReady，
// 调用方据此请求延迟结算（票据保持 pending），成功后同步完成模块 sidecar 槽位生命周期。
using QolSaveWriteFn = QolResult (*)(int32_t kind, int32_t slot, int32_t class_idx, uint32_t attempt,
                                     char* out, uint32_t capacity, uint32_t* out_size);

struct QolSaveWritePortReadApi {
    QolSaveWriteFn write;
};

struct QolSaveWritePort {
    QolVersion abi_version;
    QolSaveWritePortReadApi read;
};

static_assert(std::is_standard_layout<QolSaveWritePort>::value,
              "QolSaveWritePort must remain standard layout");
static_assert(std::is_trivially_copyable<QolSaveWritePort>::value,
              "QolSaveWritePort must remain trivially copyable");

}  // namespace qol::contracts
