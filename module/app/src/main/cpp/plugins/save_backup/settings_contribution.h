#pragma once

#include "../../contracts/host_sdk.h"

namespace qol::features::save_backup {
// action 由 catalog.invoke_action 在页面帧安全点分派，导航由 ui_panels 管理。
QolResult register_settings(const contracts::QolHostSdk* sdk);
}  // namespace qol::features::save_backup
