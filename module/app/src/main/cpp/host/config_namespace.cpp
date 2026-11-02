#include "host/config_namespace.h"

#include "host/owner_identity.h"

#include <cstring>
#include <mutex>
#include <string>

namespace qol::host {
namespace {

struct Backend {
    void* context = nullptr;
    ConfigNamespaceReadFn read = nullptr;
    ConfigNamespaceUpdateValueFn update_value = nullptr;
    ConfigNamespaceReadRegisteredFn read_registered = nullptr;
    ConfigNamespaceHttpUpdateFn update_http_request = nullptr;
    ConfigNamespaceLegacyReplaceFn legacy_replace = nullptr;
    ConfigNamespaceLegacyClearFn legacy_clear = nullptr;
    bool bound = false;
};

std::mutex g_mutex;
Backend g_backend;

QolResult fail(int32_t code, const char* text) {
    return contracts::error(code, text, static_cast<uint32_t>(std::strlen(text)));
}

bool backend_snapshot(Backend* out) {
    std::lock_guard<std::mutex> lock(g_mutex);
    *out = g_backend;
    return out->bound;
}

QolResult legacy_update_unsupported(void*, const char*, const char*, const char*, const char*, uint32_t) {
    return fail(kQolErrUnsupported, "legacy namespace replacement is unsupported");
}

QolResult legacy_registered_unavailable(void*, char*, uint32_t, uint32_t*) {
    return fail(kQolErrNotReady, "registered config backend unavailable");
}

QolResult legacy_http_unavailable(void*, const char*, const char*, uint32_t, int32_t*, char*,
                                  uint32_t, uint32_t*) {
    return fail(kQolErrNotReady, "config HTTP backend unavailable");
}

class NamespaceCallLease {
public:
    explicit NamespaceCallLease(contracts::QolOwnerToken token)
        : result_(owner_namespace_call_begin(token, &call_)) {}
    ~NamespaceCallLease() { owner_namespace_call_end(&call_); }

    const QolResult& result() const { return result_; }
    const char* plugin_id() const { return call_.plugin_id.c_str(); }

private:
    OwnerNamespaceCall call_;
    QolResult result_;
};

QolResult read_json(contracts::QolOwnerToken owner, char* out, uint32_t capacity,
                    uint32_t* out_size) {
    if (out_size != nullptr) *out_size = 0;
    if (out_size == nullptr || (capacity != 0 && out == nullptr))
        return fail(kQolErrInvalidArgument, "invalid config namespace buffer");
    NamespaceCallLease lease(owner);
    if (!contracts::succeeded(lease.result())) return lease.result();
    Backend backend;
    if (!backend_snapshot(&backend) || backend.read == nullptr)
        return fail(kQolErrNotReady, "config namespace backend unavailable");
    return backend.read(backend.context, lease.plugin_id(), out, capacity, out_size);
}

QolResult replace_json(contracts::QolOwnerToken, const char*, uint32_t) {
    return fail(kQolErrUnsupported, "namespace replacement is unsupported; use update_value");
}

QolResult clear(contracts::QolOwnerToken) {
    return fail(kQolErrUnsupported, "namespace clear is unsupported");
}

QolResult update_value(contracts::QolOwnerToken owner, const char* target_plugin_id,
                       const char* key, const char* json_value, uint32_t size) {
    if (target_plugin_id == nullptr || key == nullptr || key[0] == '\0' ||
        (size != 0 && json_value == nullptr))
        return fail(kQolErrInvalidArgument, "invalid config namespace update");
    NamespaceCallLease lease(owner);
    if (!contracts::succeeded(lease.result())) return lease.result();
    Backend backend;
    if (!backend_snapshot(&backend) || backend.update_value == nullptr)
        return fail(kQolErrNotReady, "config namespace backend unavailable");
    return backend.update_value(backend.context, lease.plugin_id(), target_plugin_id, key,
                                json_value, size);
}

QolResult read_registered(contracts::QolOwnerToken owner, char* out, uint32_t capacity,
                          uint32_t* out_size) {
    if (out_size != nullptr) *out_size = 0;
    if (out_size == nullptr || (capacity != 0 && out == nullptr))
        return fail(kQolErrInvalidArgument, "invalid registered config buffer");
    NamespaceCallLease lease(owner);
    if (!contracts::succeeded(lease.result())) return lease.result();
    if (std::strcmp(lease.plugin_id(), "core.settings") != 0)
        return fail(kQolErrBlocked, "registered config is restricted to core.settings");
    Backend backend;
    if (!backend_snapshot(&backend) || backend.read_registered == nullptr)
        return fail(kQolErrNotReady, "config namespace backend unavailable");
    return backend.read_registered(backend.context, out, capacity, out_size);
}

QolResult update_http_request(contracts::QolOwnerToken owner, const char* body,
                              uint32_t body_size, int32_t* out_http_status, char* out_body,
                              uint32_t capacity, uint32_t* out_size) {
    if (out_http_status == nullptr || out_size == nullptr ||
        (body_size != 0 && body == nullptr) || (capacity != 0 && out_body == nullptr))
        return fail(kQolErrInvalidArgument, "invalid config HTTP request buffer");
    *out_http_status = 500;
    *out_size = 0;
    NamespaceCallLease lease(owner);
    if (!contracts::succeeded(lease.result())) return lease.result();
    if (std::strcmp(lease.plugin_id(), "core.settings") != 0)
        return fail(kQolErrBlocked, "config HTTP update is restricted to core.settings");
    Backend backend;
    if (!backend_snapshot(&backend) || backend.update_http_request == nullptr)
        return fail(kQolErrNotReady, "config namespace backend unavailable");
    return backend.update_http_request(backend.context, lease.plugin_id(), body, body_size,
                                       out_http_status, out_body, capacity, out_size);
}

const contracts::QolConfigNamespaceApi kApi{
    sizeof(contracts::QolConfigNamespaceApi), contracts::kConfigNamespaceMajor,
    contracts::kConfigNamespaceMinor, &read_json, &replace_json, &clear, &update_value,
    &read_registered, &update_http_request};

}  // namespace

bool config_namespace_bind_backend(void* context, ConfigNamespaceReadFn read,
                                   ConfigNamespaceUpdateValueFn update_value_fn,
                                   ConfigNamespaceReadRegisteredFn read_registered_fn,
                                   ConfigNamespaceHttpUpdateFn update_http_request_fn) {
    if (read == nullptr || update_value_fn == nullptr || read_registered_fn == nullptr ||
        update_http_request_fn == nullptr) return false;
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_backend.bound) {
        // 幂等重试：native bootstrap 可能重复 bind，仅当完全相同时视为成功；不同后端不替换。
        return g_backend.context == context && g_backend.read == read &&
               g_backend.update_value == update_value_fn &&
               g_backend.read_registered == read_registered_fn &&
               g_backend.update_http_request == update_http_request_fn;
    }
    g_backend = {context, read, update_value_fn, read_registered_fn, update_http_request_fn,
                 nullptr, nullptr, true};
    return true;
}

bool config_namespace_bind_backend(void* context, ConfigNamespaceReadFn read,
                                   ConfigNamespaceLegacyReplaceFn legacy_replace,
                                   ConfigNamespaceLegacyClearFn legacy_clear) {
    if (read == nullptr || legacy_replace == nullptr || legacy_clear == nullptr) return false;
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_backend.bound) {
        return g_backend.context == context && g_backend.read == read &&
               g_backend.legacy_replace == legacy_replace && g_backend.legacy_clear == legacy_clear;
    }
    g_backend = {context, read, &legacy_update_unsupported, &legacy_registered_unavailable,
                 &legacy_http_unavailable, legacy_replace, legacy_clear, true};
    return true;
}

const contracts::QolConfigNamespaceApi& config_namespace_api() { return kApi; }

void config_namespace_reset_for_test() {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_backend = Backend{};
}

}  // namespace qol::host
