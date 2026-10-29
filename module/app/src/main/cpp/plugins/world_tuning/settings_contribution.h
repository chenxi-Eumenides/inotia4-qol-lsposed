#pragma once

#include "../../contracts/host_sdk.h"

// 旧 settings catalog contribution 兼容入口；feature 元数据只来自 descriptor。
namespace qol::features::world_tuning {
QolResult register_settings(const contracts::QolHostSdk* sdk);
}  // namespace qol::features::world_tuning
