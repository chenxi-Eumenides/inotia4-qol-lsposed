#pragma once

#include "../../../contracts/host_sdk.h"

namespace qol::features::core_ui {
bool debug_help_register(const contracts::QolHostSdk* sdk);
void debug_help_unregister(const contracts::QolHostSdk* sdk);
void debug_help_set_enabled(bool enabled);
}  // namespace qol::features::core_ui
