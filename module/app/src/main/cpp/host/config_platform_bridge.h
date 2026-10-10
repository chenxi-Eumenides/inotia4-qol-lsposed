#pragma once

#include "host/core_settings_config_service.h"

namespace qol::host {

bool config_platform_bind_namespace_store();
int config_store_bootstrap(const std::string& external_files_directory);
const std::string& config_store_path();
bool config_store_bootstrapped();
void config_store_mark_bootstrapped(bool completed);
void config_apply_safe_native_state(const ModuleConfig& config);
void config_apply_api_enabled(bool enabled);
CoreSettingsRuntimeApplyResult config_platform_last_runtime_apply_result();
CoreSettingsRuntimeApplyResult config_platform_last_runtime_apply_result();

}  // namespace qol::host
