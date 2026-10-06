#pragma once

#include "../../contracts/host_sdk.h"

namespace qol::features::autosell {
// action 由 catalog.invoke_action 在页面帧安全点分派，不在注册期执行。
QolResult register_settings(const contracts::QolHostSdk* sdk);
}  // namespace qol::features::autosell
