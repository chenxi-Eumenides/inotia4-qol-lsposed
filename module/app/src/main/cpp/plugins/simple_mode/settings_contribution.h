#pragma once

#include "../../contracts/host_sdk.h"

namespace qol::features::simple_mode {
QolResult register_settings(const contracts::QolHostSdk* sdk);
}  // namespace qol::features::simple_mode
