#pragma once

#include "../../../contracts/host_sdk.h"

namespace qol::features::core_ui {
bool debug_scroll_register(const contracts::QolHostSdk* sdk);
void debug_scroll_unregister(const contracts::QolHostSdk* sdk);
void debug_scroll_set_enabled(bool enabled);
}  // namespace qol::features::core_ui
