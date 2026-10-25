#pragma once

// 扩展背包声明式资源桥（D2a-3b-1）。
//
// 与 inventory_hooks 同构：wrapper / OwnershipOps 业务体仍在 plugins/extension_bag
// （资源实现收口在 claims.cpp），
// plugins/extension_bag 只经本头取得 10 条资源声明表、交还入口 hook backup、
// 驱动门禁两态，避免插件 include plugins/extension_bag 内部头。

#include <cstdint>

#include "contracts/plugin.h"
#include "contracts/item_facts.h"
#include "contracts/craft_facts.h"
#include "contracts/game_ports/extension_bag_write_port.h"
#include "contracts/game_ports/character_extras_port.h"
#include "contracts/game_ports/inventory_extras_port.h"

namespace qol::features::extension_bag {

// 10 条声明式资源声明表（静态存储，生命周期 ≥ 进程）；out_count 可空。
// 详情按钮槽所有权排在 P_EXT_DESC_OPEN_GATE 之前；Store/Mix claims 不注册。
const qol::contracts::ResourceClaim* extension_bag_claims(uint32_t* out_count);

// 入口 hook backup 交还（宿主 on_resource_ready 调用）：按资源 id 写入对应 wrapper 的
// 原函数槽；X1 指针槽 wrapper 直走 game_access 解析入口，接收即成功；未知 id 返回非 0。
int32_t extension_bag_bind_backup(const char* resource_id, void* backup);

// 门禁两态：宿主 on_enabled/on_disabled 调用；副作用与旧 set_virtual_bag_enabled 逐字一致。
bool extension_bag_set_enabled(bool enabled);
bool extension_bag_enabled();

// 插件安装期注入 Host SDK/Game Host；独立 TU 通过这些入口更新共享状态。
bool extension_bag_bind_host(const qol::contracts::QolHostSdk* sdk,
                             const qol::contracts::QolGameHost* game,
                             const qol::contracts::QolExtensionBagWritePort* write_port);
void extension_bag_bind_port_sdk(const qol::contracts::QolHostSdk* sdk);
void extension_bag_bind_character_extras(
    const qol::contracts::QolCharacterExtrasPort* port);
void extension_bag_bind_inventory_extras(
    const qol::contracts::QolInventoryExtrasPort* port);
void extension_bag_bind_item_facts(const qol::contracts::ItemFactsApi* facts);
const qol::contracts::ItemFactsApi* extension_bag_item_facts();
void extension_bag_bind_craft_facts(const qol::contracts::CraftFactsApi* facts);

}  // namespace qol::features::extension_bag
