#pragma once

#include "../../contracts/host_sdk.h"

namespace qol::features::stack_limit {
QolResult register_settings(const contracts::QolHostSdk* sdk);
}  // namespace qol::features::stack_limit
