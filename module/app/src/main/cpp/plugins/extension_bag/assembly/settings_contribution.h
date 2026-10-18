#pragma once

#include "contracts/host_sdk.h"

namespace qol::features::extension_bag {
QolResult register_settings(const contracts::QolHostSdk* sdk);
}  // namespace qol::features::extension_bag
