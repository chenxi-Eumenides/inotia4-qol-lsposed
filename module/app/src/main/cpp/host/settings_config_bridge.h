#pragma once

#include "contracts/settings_config.h"

#ifdef __ANDROID__
#include <jni.h>
#endif

namespace qol::host {

// Adapter is invoked on the host background dispatcher, never on the caller/game thread.
// It only admits work to the serial Kotlin writer; complete() is the separate receipt.
using SettingsConfigDispatch = QolResult (*)(void* context, uint64_t id, const char* key,
                                            int32_t desired);
const contracts::QolSettingsConfigApi& settings_config_api();
QolResult settings_config_install(SettingsConfigDispatch dispatch, void* context);
QolResult settings_config_publish(const int32_t* values, uint32_t count, uint64_t revision);
QolResult settings_config_complete(uint64_t id, int32_t persisted, int32_t configured,
                                   contracts::SettingsConfigApplyState apply, int32_t error_code,
                                   const char* error, const char* apply_message);
const char* settings_config_key(uint32_t index);
void settings_config_shutdown();

#ifdef __ANDROID__
bool settings_config_register_jni(JNIEnv* env, jclass bridge);
#endif

}  // namespace qol::host
