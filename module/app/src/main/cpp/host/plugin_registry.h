#pragma once

#include <cstdint>

#include "contracts/plugin.h"
#include "hooks/install.h"

// 内置静态插件注册表与声明式安装器（ARCHITECTURE.md §3.2 生命周期、§7.2 装配顺序）。
//
// 固定容量数组，不使用 STL 容器；机制原语（hook/unhook、符号解析）由宿主注入，
// 使 host 单测可脱离游戏进程。
//
// 线程纪律：注册与安装只在装配期/主线程发生；本模块不引入锁。

namespace qol::host {

struct HostDeps {
    uintptr_t (*resolve_entry)(const char* symbol, bool* out_followed);
    qol::hooks::NativeHookFn hook_fn;
    int (*unhook_fn)(void* func);
};

// 注册插件描述符（描述符、字符串与数组须为静态存储，生命周期 ≥ 进程）。
// 空 id / 重复 id / 非法描述符 / 资源登记冲突 → false。
bool register_plugin(const contracts::PluginDescriptor& plugin);

// 安装全部已注册插件；返回失败插件数（0 = 全部按要求就绪）。
int install_all(const HostDeps& deps);

// 门禁监督（主线程帧驱动）：比较带 gate 资源的配置值与登记状态，变化时同步
// hooks::registry 状态、infra::status 状态，并调用 on_enabled/on_disabled。
// 返回本轮发生状态变化的插件数。仅处理处于 kEnabled/kInstalledDisabled 的插件；幂等。
uint32_t sync_gates();

// 生命周期动作（主线程唯一执行器；命令经命令端口投递后由本函数执行）。
// action 取 contracts::kFeatureLifecycle*；状态机见 ARCHITECTURE.md §3.2。
// 前置拒绝（返回 false）：插件不存在 / 该插件有未终态事务 / 任一资源已隔离。
bool lifecycle_apply(const char* plugin_id, int32_t action);

// 帧驱动：推进在途生命周期事务并同步状态/回调；返回本轮推进数（0 = 无事可做）。
uint32_t lifecycle_step();

// 未注册返回 nullptr。
const contracts::PluginDescriptor* find_plugin(const char* id);

uint32_t plugin_count();

// 测试用：清空注册表（资源登记表另有 hooks::registry_reset_for_test）。
void plugin_registry_reset_for_test();

}  // namespace qol::host
