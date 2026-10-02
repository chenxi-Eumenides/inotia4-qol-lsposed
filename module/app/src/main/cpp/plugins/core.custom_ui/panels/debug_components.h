#pragma once

#include "../../../contracts/host_sdk.h"

namespace qol::features::core_ui {

bool debug_components_register(const contracts::QolHostSdk* sdk);
void debug_components_unregister(const contracts::QolHostSdk* sdk);
void debug_components_set_enabled(bool enabled);

}  // namespace qol::features::core_ui
