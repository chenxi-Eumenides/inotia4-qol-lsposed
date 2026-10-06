#pragma once

#include "../../contracts/host_sdk.h"

namespace qol::features::core_ui {

QolResult register_settings_category(const contracts::QolHostSdk* sdk);
QolResult register_settings(const contracts::QolHostSdk* sdk);

}  // namespace qol::features::core_ui
