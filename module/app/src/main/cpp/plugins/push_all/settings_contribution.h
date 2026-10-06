#pragma once

#include "../../contracts/host_sdk.h"

namespace qol::features::push_all {
QolResult register_settings(const contracts::QolHostSdk* sdk);
}  // namespace qol::features::push_all
