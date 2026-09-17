#pragma once

#include "contracts/plugin.h"

// 存档管理器备份插件清单入口（ARCHITECTURE.md §3.3）：零资源、常驻。
// 删档 PtrHook 属过渡期 legacy 机制（game_ptr_hook），由 install 执行；
// 待 patch/ptr_slot（B2a-3，随 D2）转声明式资源后改由宿主通用执行。

namespace qol::features::save_backup {

const contracts::PluginDescriptor& plugin_descriptor();

}  // namespace qol::features::save_backup
