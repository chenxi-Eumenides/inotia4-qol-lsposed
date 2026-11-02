#pragma once

#include <cstdint>

#include "contracts/game_ports/item_inventory_state_write_port.h"
#include "contracts/game_ports/item_money_write_port.h"
#include "contracts/game_ports/item_port.h"
#include "contracts/host_sdk.h"

namespace qol::features::core_inventory {

// 插件生命周期绑定的 Host SDK 与 item port；读侧 HTTP（快照刷新/查询）与 hook 层共用。
// 由 plugin_install 绑定、plugin_deactivate 置空。
extern const contracts::QolHostSdk* g_sdk;
extern const contracts::QolItemPort* g_port;
// 原生库存状态写与金钱写端口：transfer 层的 import/remove 原语与 HTTP 写侧迁移共用。
// 由 plugin_install 校验并绑定、plugin_deactivate 置空；on_disabled 保留绑定，
// 否则 disable 后 re-enable 前写命令/transfer 无端口可用。
extern const contracts::QolItemInventoryStateWritePort* g_item_state_write;
extern const contracts::QolItemMoneyWritePort* g_item_money_write;

namespace bags {

// 原生物理袋扫描上界（不含）：原版袋数 + C18 探测到的追加袋数，由 item port 统一夹取。
// 由 plugin lifecycle 在端口就绪后写入；hook 层只读，避免把端口依赖带进机制层。
// 与 core.item 的同名实现保持暂存重复：三插件拆分期内两侧各自持有扫描上界。
int native_bag_limit();
void set_native_bag_limit(int32_t limit);

}  // namespace bags

}  // namespace qol::features::core_inventory
