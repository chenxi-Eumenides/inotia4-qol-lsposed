#pragma once

#include "contracts/config_namespace.h"
#include "host/core_settings_config_service.h"

namespace qol::host {

using ConfigNamespaceReadFn = QolResult (*)(void* context, const char* plugin_id, char* out_json,
                                            uint32_t capacity, uint32_t* out_size);
using ConfigNamespaceUpdateValueFn = QolResult (*)(void* context, const char* caller_plugin_id,
                                                   const char* target_plugin_id, const char* key,
                                                   const char* json_value, uint32_t size);
using ConfigNamespaceReadRegisteredFn = QolResult (*)(void* context, char* out_json,
                                                      uint32_t capacity, uint32_t* out_size);
using ConfigNamespaceHttpUpdateFn = QolResult (*)(void* context, const char* caller_plugin_id,
                                                 const char* body, uint32_t body_size,
                                                 int32_t* out_http_status, char* out_body,
                                                 uint32_t capacity, uint32_t* out_size);
using ConfigNamespaceLegacyReplaceFn = QolResult (*)(void* context, const char* plugin_id,
                                                     const char* json, uint32_t size);
using ConfigNamespaceLegacyClearFn = QolResult (*)(void* context, const char* plugin_id);

bool config_namespace_bind_backend(void* context, ConfigNamespaceReadFn read,
                                   ConfigNamespaceUpdateValueFn update_value,
                                   ConfigNamespaceReadRegisteredFn read_registered,
                                   ConfigNamespaceHttpUpdateFn update_http_request);
// Transitional backend binding overload keeps older Host-only test fixtures linkable. The legacy
// write callbacks are never invoked; the corresponding API slots return Unsupported.
bool config_namespace_bind_backend(void* context, ConfigNamespaceReadFn read,
                                   ConfigNamespaceLegacyReplaceFn legacy_replace,
                                   ConfigNamespaceLegacyClearFn legacy_clear);
const contracts::QolConfigNamespaceApi& config_namespace_api();
void config_namespace_reset_for_test();

}  // namespace qol::host
