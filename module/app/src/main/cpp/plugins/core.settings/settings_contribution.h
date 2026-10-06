#pragma once

#include "../../contracts/host_sdk.h"

namespace qol::features::core_settings {
QolResult register_categories(const contracts::QolHostSdk* sdk);
// 仅登记常驻声明；缺少可选 SDK 尾接口返回 unsupported，不影响功能安装。
QolResult register_settings(const contracts::QolHostSdk* sdk);
}  // namespace qol::features::core_settings
