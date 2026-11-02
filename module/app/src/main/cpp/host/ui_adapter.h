#pragma once

#include "contracts/ui.h"

namespace qol::host {

// 返回进程生命周期内有效的 host UI adapter 实例。
const contracts::QolUiAdapter* ui_adapter();

}  // namespace qol::host
