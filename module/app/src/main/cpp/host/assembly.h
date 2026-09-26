#pragma once

#include <cstdint>

// 插件装配入口（ARCHITECTURE.md §7.2 装配顺序）。幂等；由 nativeInit 在 bridge 就绪后调用。

namespace qol::host {

// 注入帧时钟、注册命令排空帧任务、注册并安装内置静态插件。
// 返回 true = 宿主装配完成；插件级安装失败保留在 AssemblyReport 中，不阻止独立插件启动。
bool assembly_init();

// T2h-B'：host↔infra 内部注入面（A1）。装配早期向 game_cache 注入 legacy JSON builder
// （K1：与现状同一实现，行为/线程逐字不变；api/native TU 不删）。须在 frame_cache_start 前调用。
void install_legacy_cache_builders();

// T2h-B1（v3 A-simple）：把 player/party/skills 三槽 builder 覆盖为 owner 适配器
// （character.state 现取、失败逐字 fallback legacy）。须在 install_legacy_cache_builders()
// 之后、frame_cache_start() 之前调用；inventory/mercenaries 槽不在本批范围。
void install_owner_cache_builders();

struct AssemblyReport {
    uint32_t registered_plugins;
    uint32_t registration_failures;
    uint32_t install_failures;
    bool kernel_only;
    bool initialized;  // 宿主装配流程完成；不表示所有插件均成功。
};

AssemblyReport assembly_report();

}  // namespace qol::host
