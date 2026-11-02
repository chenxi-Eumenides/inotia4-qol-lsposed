#pragma once

#include "../../contracts/plugin.h"

// 存档管理器备份插件清单入口（ARCHITECTURE.md §3.3）：1 条 kPtrSlot 资源（常驻）。
// 删档回调 SaveSlot_Delete 的 GOT 槽以声明式资源登记，机制由宿主通用执行
// （站点经 game/access/ptr_slots 解析，见 patch/ptr_slot）；原函数入口经
// on_resource_ready 交还 wrapper。

namespace qol::features::save_backup {

const contracts::PluginDescriptor& plugin_descriptor();

}  // namespace qol::features::save_backup
