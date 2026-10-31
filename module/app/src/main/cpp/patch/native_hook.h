#pragma once

// LSPosed native hook API 注入点。此前由 core.item 导出；迁移到 patch 层后，
// 宿主与各插件不再依赖某个业务插件来获得 hook/unhook 原语。

#include <cstdint>

using NativeHookFunType = int (*)(void* func, void* replace, void** backup);
using NativeUnhookFunType = int (*)(void* func);

struct NativeAPIEntries {
    uint32_t version;
    NativeHookFunType hook_func;
    NativeUnhookFunType unhook_func;
};

using NativeOnModuleLoaded = void (*)(const char* name, void* handle);

NativeHookFunType native_hook_func();
NativeUnhookFunType native_unhook_func();

extern "C" [[gnu::visibility("default")]] [[gnu::used]]
NativeOnModuleLoaded native_init(const NativeAPIEntries* entries);
