#pragma once

#include <cstdint>
#include <cstddef>

#include "owner_token.h"
#include "result.h"

namespace qol::contracts {

constexpr uint16_t kConfigNamespaceMajor = 1;
constexpr uint16_t kConfigNamespaceMinor = 2;
constexpr uint16_t kConfigNamespaceLegacyMinor = 0;
constexpr uint16_t kConfigNamespaceUpdateMinor = 1;
constexpr uint16_t kConfigNamespaceHttpMinor = 2;

struct QolConfigNamespaceApi {
    uint32_t size;
    uint16_t major;
    uint16_t minor;
    // Reads the active caller's complete registered namespace; there is no target parameter.
    QolResult (*read_json)(QolOwnerToken owner, char* out_json, uint32_t capacity,
                           uint32_t* out_size);
    // Frozen v1.0 slots; v1.1 hosts return Unsupported without dispatching these writes.
    QolResult (*replace_json)(QolOwnerToken owner, const char* json, uint32_t size);
    QolResult (*clear)(QolOwnerToken owner);
    // v1.1 only: caller is derived from owner token; target is explicit.
    QolResult (*update_value)(QolOwnerToken owner, const char* target_plugin_id,
                              const char* key, const char* json_value, uint32_t size);
    // v1.1 only: restricted by Host to the active core.settings owner.
    QolResult (*read_registered)(QolOwnerToken owner, char* out_json, uint32_t capacity,
                                 uint32_t* out_size);
    // v1.2 HTTP adapter. The Host parses and commits the request under the same Store transaction.
    QolResult (*update_http_request)(QolOwnerToken owner, const char* body, uint32_t body_size,
                                    int32_t* out_http_status, char* out_body, uint32_t capacity,
                                    uint32_t* out_size);
};

inline const QolConfigNamespaceApi* config_namespace_update_if_available(
    const QolConfigNamespaceApi* api) {
    constexpr std::size_t required = offsetof(QolConfigNamespaceApi, update_value) +
                                     sizeof(QolConfigNamespaceApi::update_value);
    if (api == nullptr || api->major != kConfigNamespaceMajor ||
        api->minor < kConfigNamespaceUpdateMinor || api->size < required ||
        api->update_value == nullptr) return nullptr;
    return api;
}

inline const QolConfigNamespaceApi* config_namespace_registered_if_available(
    const QolConfigNamespaceApi* api) {
    constexpr std::size_t required = offsetof(QolConfigNamespaceApi, read_registered) +
                                     sizeof(QolConfigNamespaceApi::read_registered);
    if (api == nullptr || api->major != kConfigNamespaceMajor ||
        api->minor < kConfigNamespaceUpdateMinor || api->size < required ||
        api->read_registered == nullptr) return nullptr;
    return api;
}

inline const QolConfigNamespaceApi* config_namespace_http_if_available(
    const QolConfigNamespaceApi* api) {
    constexpr std::size_t required = offsetof(QolConfigNamespaceApi, update_http_request) +
                                     sizeof(QolConfigNamespaceApi::update_http_request);
    if (api == nullptr || api->major != kConfigNamespaceMajor ||
        api->minor < kConfigNamespaceHttpMinor || api->size < required ||
        api->update_http_request == nullptr) return nullptr;
    return api;
}

}  // namespace qol::contracts
