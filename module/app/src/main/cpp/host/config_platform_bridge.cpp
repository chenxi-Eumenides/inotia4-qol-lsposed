#include "host/config_platform_bridge.h"

#include "host/config_namespace.h"
#include "host/settings_config_bridge.h"
#include "host/http_dispatch.h"
#include "contracts/commands.h"
#include "infra/config.h"
#include "infra/commands.h"
#include "core/native/game_cache.h"
#include "infra/log/qol_log.h"

#include <algorithm>
#include <cstring>
#include <mutex>
#include <string>

namespace qol::host {
namespace {

std::mutex g_bootstrap_mutex;
std::string g_bootstrap_path;
bool g_bootstrap_attempted = false;
bool g_bootstrap_completed = false;

QolResult to_qol_result(const ConfigStoreResult& result) {
    if (result.ok()) return contracts::ok();
    int32_t code = kQolErrInternal;
    switch (result.error) {
        case qol::features::core_settings::ConfigStoreError::kInvalidArgument:
        case qol::features::core_settings::ConfigStoreError::kValidation:
            code = kQolErrInvalidArgument;
            break;
        case qol::features::core_settings::ConfigStoreError::kNotFound:
            code = kQolErrNotFound;
            break;
        case qol::features::core_settings::ConfigStoreError::kNotReady:
            code = kQolErrNotReady;
            break;
        case qol::features::core_settings::ConfigStoreError::kNone:
        case qol::features::core_settings::ConfigStoreError::kPersistence:
            code = kQolErrInternal;
            break;
    }
    thread_local std::string message;
    message = result.message;
    return contracts::error(code, message.c_str(), static_cast<uint32_t>(message.size()));
}

void append_quoted(std::string* out, const std::string& text) {
    static constexpr char hex[] = "0123456789abcdef";
    out->push_back('"');
    for (unsigned char ch : text) {
        switch (ch) {
            case '"': *out += "\\\""; break;
            case '\\': *out += "\\\\"; break;
            case '\b': *out += "\\b"; break;
            case '\f': *out += "\\f"; break;
            case '\n': *out += "\\n"; break;
            case '\r': *out += "\\r"; break;
            case '\t': *out += "\\t"; break;
            default:
                if (ch < 0x20) {
                    *out += "\\u00";
                    out->push_back(hex[ch >> 4]);
                    out->push_back(hex[ch & 0x0f]);
                } else {
                    out->push_back(static_cast<char>(ch));
                }
        }
    }
    out->push_back('"');
}

void append_value(std::string* out, const JsonValue& value) {
    switch (value.kind) {
        case JsonValue::Kind::kNull: *out += "null"; break;
        case JsonValue::Kind::kBoolean: *out += value.boolean ? "true" : "false"; break;
        case JsonValue::Kind::kNumber: *out += value.scalar; break;
        case JsonValue::Kind::kString: append_quoted(out, value.scalar); break;
        case JsonValue::Kind::kArray:
            out->push_back('[');
            for (std::size_t i = 0; i < value.array.size(); ++i) {
                if (i != 0) out->push_back(',');
                append_value(out, value.array[i]);
            }
            out->push_back(']');
            break;
        case JsonValue::Kind::kObject:
            out->push_back('{');
            for (std::size_t i = 0; i < value.object.size(); ++i) {
                if (i != 0) out->push_back(',');
                append_quoted(out, value.object[i].first);
                out->push_back(':');
                append_value(out, value.object[i].second);
            }
            out->push_back('}');
            break;
    }
}

const JsonValue* object_member(const JsonValue& object, const char* name) {
    if (object.kind != JsonValue::Kind::kObject) return nullptr;
    const auto found = std::find_if(object.object.begin(), object.object.end(),
        [name](const auto& entry) { return entry.first == name; });
    return found == object.object.end() ? nullptr : &found->second;
}

QolResult write_http_body(int32_t status, const std::string& body, int32_t* out_status,
                          char* out_body, uint32_t capacity, uint32_t* out_size) {
    if (out_status == nullptr || out_size == nullptr || (capacity != 0 && out_body == nullptr))
        return contracts::error(kQolErrInvalidArgument, "invalid config HTTP response", 29);
    *out_status = status;
    *out_size = 0;
    if (body.size() > capacity)
        return contracts::error(kQolErrBlocked, "config HTTP response buffer too small", 37);
    if (!body.empty()) std::memcpy(out_body, body.data(), body.size());
    *out_size = static_cast<uint32_t>(body.size());
    return contracts::ok();
}

QolResult write_http_error(int32_t status, const std::string& message, int32_t* out_status,
                           char* out_body, uint32_t capacity, uint32_t* out_size) {
    std::string body = "{\"ok\":false,\"error\":";
    append_quoted(&body, message);
    body.push_back('}');
    return write_http_body(status, body, out_status, out_body, capacity, out_size);
}

QolResult namespace_http_update(void*, const char* caller_plugin_id, const char* body,
                                uint32_t body_size, int32_t* out_status, char* out_body,
                                uint32_t capacity, uint32_t* out_size) {
    if (caller_plugin_id == nullptr || std::strcmp(caller_plugin_id, "core.settings") != 0)
        return write_http_error(403, "config owner forbidden", out_status, out_body, capacity,
                                out_size);
    if (body == nullptr || body_size == 0 || body_size > 1024 * 1024)
        return write_http_error(400, "bad request", out_status, out_body, capacity, out_size);
    JsonValue envelope;
    const std::string request(body, body_size);
    const ConfigStoreResult parsed = qol::features::core_settings::parse_json_document(request, &envelope);
    if (!parsed.ok() || envelope.kind != JsonValue::Kind::kObject || envelope.object.size() != 3)
        return write_http_error(400, "bad request", out_status, out_body, capacity, out_size);
    const JsonValue* target = object_member(envelope, "plugin_id");
    const JsonValue* key = object_member(envelope, "key");
    const JsonValue* value = object_member(envelope, "value");
    if (target == nullptr || target->kind != JsonValue::Kind::kString ||
        key == nullptr || key->kind != JsonValue::Kind::kString || value == nullptr ||
        key->scalar.empty()) {
        return write_http_error(400, "bad request", out_status, out_body, capacity, out_size);
    }
    std::string serialized_value;
    append_value(&serialized_value, *value);
    const CoreSettingsConfigCommit commit = core_settings_config_service().update_namespace_key_commit(
        caller_plugin_id, target->scalar, key->scalar, serialized_value);
    if (!commit.committed) {
        int32_t status = 500;
        std::string message = "config save failed";
        switch (commit.result.error) {
            case qol::features::core_settings::ConfigStoreError::kInvalidArgument:
            case qol::features::core_settings::ConfigStoreError::kValidation:
                status = 400;
                message = commit.result.message;
                break;
            case qol::features::core_settings::ConfigStoreError::kNotFound:
                status = 404;
                message = commit.result.message;
                break;
            case qol::features::core_settings::ConfigStoreError::kNotReady:
                status = 503;
                message = "configuration unavailable";
                break;
            case qol::features::core_settings::ConfigStoreError::kNone:
            case qol::features::core_settings::ConfigStoreError::kPersistence:
                break;
        }
        return write_http_error(status, message, out_status, out_body, capacity, out_size);
    }
    std::string response = "{\"ok\":true,\"plugin_id\":";
    append_quoted(&response, target->scalar);
    response += ",\"key\":";
    append_quoted(&response, key->scalar);
    response += ",\"value\":";
    append_value(&response, *value);
    response.push_back('}');
    return write_http_body(200, response, out_status, out_body, capacity, out_size);
}

QolResult namespace_read(void*, const char* plugin_id, char* out_json,
                         uint32_t capacity, uint32_t* out_size) {
    if (plugin_id == nullptr || out_size == nullptr || (capacity != 0 && out_json == nullptr))
        return contracts::error(kQolErrInvalidArgument, "invalid config namespace buffer",
                                sizeof("invalid config namespace buffer") - 1);
    *out_size = 0;
    JsonValue value;
    const ConfigStoreResult result = core_settings_config_service().read_namespace(plugin_id, &value);
    if (!result.ok()) return to_qol_result(result);
    std::string json;
    append_value(&json, value);
    if (json.size() > capacity) {
        return contracts::error(kQolErrBlocked, "config namespace response too large",
                                sizeof("config namespace response too large") - 1);
    }
    if (!json.empty()) std::memcpy(out_json, json.data(), json.size());
    *out_size = static_cast<uint32_t>(json.size());
    return contracts::ok();
}

QolResult namespace_update_value(void*, const char* caller_plugin_id, const char* target_plugin_id,
                                const char* key, const char* json, uint32_t size) {
    if (caller_plugin_id == nullptr || target_plugin_id == nullptr || key == nullptr ||
        (size != 0 && json == nullptr))
        return contracts::error(kQolErrInvalidArgument, "invalid config namespace JSON",
                                sizeof("invalid config namespace JSON") - 1);
    const ConfigStoreResult result = core_settings_config_service().update_namespace_key(
        caller_plugin_id, target_plugin_id, key, std::string(json == nullptr ? "" : json, size));
    return to_qol_result(result);
}

QolResult namespace_read_registered(void*, char* out_json, uint32_t capacity,
                                    uint32_t* out_size) {
    return to_qol_result(core_settings_config_service().read_registered_json(
        out_json, capacity, out_size));
}

}  // namespace

bool config_platform_bind_namespace_store() {
    auto& service = core_settings_config_service();
    if (service.state() == CoreSettingsConfigServiceState::kUninitialized) return false;
    return config_namespace_bind_backend(nullptr, &namespace_read, &namespace_update_value,
                                         &namespace_read_registered, &namespace_http_update);
}

int config_store_bootstrap(const std::string& directory) {
    std::lock_guard<std::mutex> bootstrap_lock(g_bootstrap_mutex);
    if (g_bootstrap_attempted && directory != g_bootstrap_path) return -1;
    // Path-only: the production feature controller performs Store init/seed after every
    // descriptor is registered (assembly), never on this JNI caller thread.
    g_bootstrap_path = directory;
    g_bootstrap_attempted = true;
    return 0;
}

const std::string& config_store_path() {
    std::lock_guard<std::mutex> lock(g_bootstrap_mutex);
    return g_bootstrap_path;
}

bool config_store_bootstrapped() {
    std::lock_guard<std::mutex> lock(g_bootstrap_mutex);
    return g_bootstrap_completed;
}

void config_store_mark_bootstrapped(bool completed) {
    std::lock_guard<std::mutex> lock(g_bootstrap_mutex);
    g_bootstrap_completed = completed;
}

void config_apply_safe_native_state(const ModuleConfig& config) {
    http_dispatch_set_op_enabled(config.op_enabled);
    qol_log_set_debug_enabled(config.debug_log_enabled);
}

void config_apply_api_enabled(bool enabled) {
#if !QOL_KERNEL_ONLY
    if (enabled) {
        frame_cache_start();
    } else {
        frame_cache_stop();
    }
#else
    (void)enabled;
#endif
}

CoreSettingsRuntimeApplyResult config_platform_last_runtime_apply_result() {
    return core_settings_config_service().last_runtime_apply_result();
}

}  // namespace qol::host
