#pragma once

#include <cstdint>

#include "contracts/game_host.h"
#include "contracts/game_ports/item_inventory_state_write_port.h"
#include "contracts/game_ports/item_money_write_port.h"
#include "contracts/game_ports/item_port.h"
#include "contracts/host_sdk.h"

namespace qol::features::core_inventory {

// 插件生命周期绑定的 Host SDK、game host 与 item port；读侧 HTTP（快照刷新/查询）与
// hook 层共用。由 plugin_install 绑定、plugin_deactivate 置空。
extern const contracts::QolHostSdk* g_sdk;
extern const contracts::QolGameHost* g_game;
extern const contracts::QolItemPort* g_port;
// 原生库存状态写与金钱写端口：transfer 层的 import/remove 原语与 HTTP 写侧迁移共用。
// 由 plugin_install 校验并绑定、plugin_deactivate 置空；on_disabled 保留绑定，
// 否则 disable 后 re-enable 前写命令/transfer 无端口可用。
extern const contracts::QolItemInventoryStateWritePort* g_item_state_write;
extern const contracts::QolItemMoneyWritePort* g_item_money_write;

// 当前变体是否仍把 C18 追加袋（space=1）作为自动入库目标。C18L v603 移除了 C18 背包，
// attach 时经模块已有的 game_identity 接口判定（series=c18 且 version=v603）后关闭；
// 其余变体与未知后续版本保持 true。只影响自动落槽与空槽统计，显式 move/transfer 不受影响。
extern bool g_c18_native_bag_auto_save;

namespace bags {

// 原生物理袋扫描上界（不含）：原版袋数 + C18 探测到的追加袋数，由 item port 统一夹取。
// 由 plugin lifecycle 在端口就绪后写入；hook 层只读，避免把端口依赖带进机制层。
// 与 core.item 的同名实现保持暂存重复：三插件拆分期内两侧各自持有扫描上界。
int native_bag_limit();
void set_native_bag_limit(int32_t limit);

}  // namespace bags

}  // namespace qol::features::core_inventory
