#pragma once

#include <cstdint>

// 合成器自定义配方（custom-craft-recipe §4.4/§4.5/§4.6）：UIMix type 3 面板的三处函数级 hook
// （放料改写 / 合成按钮前置校验 / 产物改写）与表注入的安装入口、总开关。
//
// hook 全部装在原函数入口，按 type/mixType 门控；不改 gemcraft 占用的 4 个 GOT 槽。
// 安装时机：nativeInit bridge_init 后调用（幂等，可后续重试）；启用时亦触发安装尝试。

// 设置总开关（JNI 入口调用）。启用即触发安装尝试与表注入；bridge 未就绪时 QOL_LOG_WARN 延迟。
bool set_custom_recipe_enabled(bool enabled);

// 当前总开关状态。
bool custom_recipe_enabled();

// 三处 hook 安装（幂等、bridge_ready 门控）。成功安装返回 true。
bool custom_recipe_ui_install_if_ready();
