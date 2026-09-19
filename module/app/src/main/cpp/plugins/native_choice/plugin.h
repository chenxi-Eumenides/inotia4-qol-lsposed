#pragma once

#include "../../contracts/plugin.h"

// 原生选择框插件清单入口（ARCHITECTURE.md §3.3）：2 条 kRequired/kFailClosed 常驻资源，
// 并向消费者提供 `ui_choice` 能力（v1.0）。
//
// 资源与旧 raw 的对应：
//   · hook:UIChoice_Init            ← 旧 install_init_hook_locked（LSPosed NativeHook）
//   · ptrslot:g_uichoice_button_list_exe_got ← 旧 install_button_hook_locked（PtrHook 槽）
// 旧「按钮槽失败则手动 unhook init」由宿主事务逆序回滚承担。

namespace qol::features::native_choice {

const contracts::PluginDescriptor& plugin_descriptor();
const contracts::QolHostSdk* host_sdk();
const contracts::QolGameHost* game_host();

}  // namespace qol::features::native_choice
