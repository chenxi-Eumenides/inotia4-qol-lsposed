#pragma once

#include "../../contracts/host_sdk.h"

namespace qol::features::custom_recipe {
QolResult register_settings(const contracts::QolHostSdk* sdk);
}  // namespace qol::features::custom_recipe
