#pragma once

// 扩展背包声明式资源桥（D2a-3b-1）。
//
// 与 inventory_hooks 同构：wrapper / OwnershipOps 业务体仍在 plugins/extension_bag
// （本桥实现收口在 game_ui_virbag.cpp 尾部的 extension_bag_claims.inc），
// plugins/extension_bag 只经本头取得 8 条资源声明表、交还入口 hook backup、
// 驱动门禁两态，避免插件 include plugins/extension_bag 内部头。

#include <cstdint>

#include "contracts/extension_bag.h"
#include "contracts/plugin.h"
#include "contracts/item_facts.h"
#include "contracts/game_ports/extension_bag_write_port.h"

namespace qol::features::extension_bag {

// 8 条声明式资源声明表（静态存储，生命周期 ≥ 进程）；out_count 可空。
// 顺序：M1/M2 入口 hook → P1–P4 PopupState 占用 → S1 desc 槽命名空间 → X1 move_merge 指针槽。
const qol::contracts::ResourceClaim* extension_bag_claims(uint32_t* out_count);

// 入口 hook backup 交还（宿主 on_resource_ready 调用）：按资源 id 写入对应 wrapper 的
// 原函数槽；X1 指针槽 wrapper 直走 game_access 解析入口，接收即成功；未知 id 返回非 0。
int32_t extension_bag_bind_backup(const char* resource_id, void* backup);

// 门禁两态：宿主 on_enabled/on_disabled 调用；副作用与旧 set_virtual_bag_enabled 逐字一致。
bool extension_bag_set_enabled(bool enabled);
bool extension_bag_enabled();

// 插件安装期注入 Host SDK/Game Host。game_ui_virtbag.cpp 是 unity TU，其 inc
// 直接读取这里缓存的句柄；独立 TU 由各自的 bridge setter 缓存所需表。
bool extension_bag_bind_host(const qol::contracts::QolHostSdk* sdk,
                             const qol::contracts::QolGameHost* game,
                             const qol::contracts::QolExtensionBagWritePort* write_port);
void extension_bag_bind_port_sdk(const qol::contracts::QolHostSdk* sdk);
const qol::contracts::QolGameHost* extension_bag_game_host();
void extension_bag_bind_item_facts(const qol::contracts::ItemFactsApi* facts);
const qol::contracts::ItemFactsApi* extension_bag_item_facts();
const qol::contracts::QolHostSdk* extension_bag_host_sdk();

// 能力 v1.0 接口表（ExtensionBagApi，37 函数）：成员逐条转发既有 extension_bag_* /
// virtual_bag_* port 实现（实现收口 plugins/extension_bag/extension_bag_port.cpp）。
// 静态存储、生命周期 ≥ 进程；provides 于插件 enabled 时注册、revoking 时注销。
const qol::contracts::ExtensionBagApi* extension_bag_api_table();

}  // namespace qol::features::extension_bag
