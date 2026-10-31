#include "native_hook.h"

#include <cstring>
#include <mutex>

#include "infra/log/qol_log.h"

namespace {

constexpr uint32_t kNativeApiVersion = 2;

std::mutex g_hook_mutex;
NativeHookFunType g_hook_func = nullptr;
NativeUnhookFunType g_unhook_func = nullptr;

void bind_api(const NativeAPIEntries* entries) {
    if (entries == nullptr || entries->hook_func == nullptr ||
        entries->unhook_func == nullptr) {
        QOL_LOG_ERROR(QolDomain::kCore, "native_init rejected reason=invalid_api");
        return;
    }
    std::lock_guard<std::mutex> lock(g_hook_mutex);
    if (entries->version < kNativeApiVersion) {
        QOL_LOG_ERROR(QolDomain::kCore, "native_init rejected version=%u required=%u",
                      entries->version, kNativeApiVersion);
        return;
    }
    g_hook_func = entries->hook_func;
    g_unhook_func = entries->unhook_func;
    QOL_LOG_INFO(QolDomain::kCore, "native_init bound api_version=%u hook=%p",
                 entries->version, reinterpret_cast<void*>(g_hook_func));
}

void on_module_loaded(const char* name, void*) {
    if (name == nullptr || std::strstr(name, "libgame.so") == nullptr) return;
    QOL_LOG_INFO(QolDomain::kCore, "native module loaded name=%s", name);
}

}  // namespace

NativeHookFunType native_hook_func() {
    std::lock_guard<std::mutex> lock(g_hook_mutex);
    return g_hook_func;
}

NativeUnhookFunType native_unhook_func() {
    std::lock_guard<std::mutex> lock(g_hook_mutex);
    return g_unhook_func;
}

extern "C" [[gnu::visibility("default")]] [[gnu::used]]
NativeOnModuleLoaded native_init(const NativeAPIEntries* entries) {
    bind_api(entries);
    return on_module_loaded;
}
