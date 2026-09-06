#pragma once

#include <cstdint>

using NativeHookFunType = int (*)(void* func, void* replace, void** backup);
using NativeUnhookFunType = int (*)(void* func);

struct NativeAPIEntries {
    uint32_t version;
    NativeHookFunType hook_func;
    NativeUnhookFunType unhook_func;
};

using NativeOnModuleLoaded = void (*)(const char* name, void* handle);

// 使用 LSPosed Native Hook API 提供的 backup；扩展对象由逻辑库存适配器处理，原版对象回退到 backup。

void inventory_native_hook_on_api(const NativeAPIEntries* entries);
void inventory_native_hook_on_module_loaded(const char* name, void* handle);
void inventory_native_hook_install_if_ready();

extern "C" [[gnu::visibility("default")]] [[gnu::used]]
NativeOnModuleLoaded native_init(const NativeAPIEntries* entries);
