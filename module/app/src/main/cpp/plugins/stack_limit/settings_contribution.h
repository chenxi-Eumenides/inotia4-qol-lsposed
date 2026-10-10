#pragma once

#include "../../contracts/host_sdk.h"

namespace qol::features::stack_limit {
// Retired legacy catalog contribution: metadata comes from descriptor.features.
QolResult register_settings(const contracts::QolHostSdk* sdk);
}  // namespace qol::features::stack_limit
