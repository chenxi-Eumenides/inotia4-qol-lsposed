#pragma once

#include <cstdint>
#include <type_traits>

#include "../result.h"
#include "../version.h"

namespace qol::contracts {

// core.ui_state owner 的对话框/交互写端点类别。
constexpr int32_t kQolChoiceWriteDialogSelect = 0;  // POST /api/ui/dialog/select（action, index）
constexpr int32_t kQolChoiceWriteNpcInteract = 1;   // POST /api/ui/start_interact

// 执行对话写并写出迁移前的结果 JSON（原始 op JSON；失败为 {"ok":false,...}）。
// 必须在游戏主线程调用（由 owner 结果处理器执行）。
// action 仅 kQolChoiceWriteDialogSelect 使用（可为空串）；index 同理。
using QolChoiceWriteFn = QolResult (*)(int32_t kind, const char* action, int32_t index, char* out,
                                       uint32_t capacity, uint32_t* out_size);

struct QolChoiceWritePortReadApi {
    QolChoiceWriteFn write;
};

struct QolChoiceWritePort {
    QolVersion abi_version;
    QolChoiceWritePortReadApi read;
};

static_assert(std::is_standard_layout<QolChoiceWritePort>::value,
              "QolChoiceWritePort must remain standard layout");
static_assert(std::is_trivially_copyable<QolChoiceWritePort>::value,
              "QolChoiceWritePort must remain trivially copyable");

}  // namespace qol::contracts
