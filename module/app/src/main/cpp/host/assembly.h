#pragma once

#include <cstdint>

// 插件装配入口（ARCHITECTURE.md §7.2 装配顺序）。幂等；由 nativeInit 在 bridge 就绪后调用。

namespace qol::host {

// 注入帧时钟、注册命令排空帧任务、注册并安装内置静态插件。
// 返回 true = 全部插件按要求就绪。
bool assembly_init();

struct AssemblyReport {
    uint32_t registered_plugins;
    uint32_t registration_failures;
    uint32_t install_failures;
    bool kernel_only;
    bool initialized;
};

AssemblyReport assembly_report();

}  // namespace qol::host
