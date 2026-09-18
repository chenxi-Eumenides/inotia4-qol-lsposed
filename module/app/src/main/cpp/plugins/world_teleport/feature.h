#pragma once

#include <cstdint>

namespace qol::features::world_teleport {

enum class InstallDisposition : uint8_t {
    kInstalled = 0,
    kUnsupported = 1,
    kFailed = 2,
};

constexpr bool install_succeeded(InstallDisposition disposition) {
    return disposition == InstallDisposition::kInstalled;
}

constexpr const char* install_error(InstallDisposition disposition) {
    switch (disposition) {
        case InstallDisposition::kInstalled:
            return nullptr;
        case InstallDisposition::kUnsupported:
            return "unsupported: world teleport entry instruction fingerprint";
        case InstallDisposition::kFailed:
            return "install failed: world teleport entry patch unavailable";
    }
    return "install failed: world teleport entry patch unavailable";
}

// 世界传送 4 选项插件业务层：拦截地图名横幅尾部，改为原生 UICHOICE 面板。
// 入口指令 patch（UIPlay_CallMapName 尾部）由 plugin.cpp 声明，宿主机制层负责安装；本层
// 只做面板/确认/扣费/切图的业务编排。
// 选择框组件经能力契约 `ui_choice`（provider=plugins/native_choice）消费，每次现取、不缓存。

// UIPlay_CallMapName 尾部 trampoline 调用的业务入口（游戏主线程）：非世界态忽略，
// 世界态请求打开选择面板。
void world_teleport_call_map_name();

}  // namespace qol::features::world_teleport
