#pragma once

#include "contracts/settings_config.h"
#include "host/core_settings_config_service.h"

#ifdef __ANDROID__
#include <jni.h>
#endif

namespace qol::host {

// Adapter is invoked on the host background dispatcher, never on the caller/game thread.
using SettingsConfigDispatch = QolResult (*)(void* context, contracts::QolOwnerToken owner, uint64_t id,
                                             const char* key, int32_t desired,
                                             int32_t configured_before);
using SettingsConfigApplyResultProvider = CoreSettingsRuntimeApplyResult (*)();
const contracts::QolSettingsConfigApi& settings_config_api();
QolResult settings_config_install(SettingsConfigDispatch dispatch, void* context);
QolResult settings_config_install_native();
void settings_config_set_apply_result_provider(SettingsConfigApplyResultProvider provider);
QolResult settings_config_publish(const int32_t* values, uint32_t count, uint64_t revision);
QolResult settings_config_complete(uint64_t id, int32_t persisted, int32_t configured,
                                   contracts::SettingsConfigApplyState apply, int32_t error_code,
                                   const char* error, const char* apply_message);
const char* settings_config_key(uint32_t index);
void settings_config_shutdown();

}  // namespace qol::host
