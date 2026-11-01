#pragma once

#include <cstdint>

#include "contracts/plugin.h"

// UIEquip item-proc wrapper 与原生物品合并门禁（自 core.item/hooks/merge_hook 迁入）。
// 依赖全部取自 extension_bag 共享状态：Host SDK 与 item.facts 走 runtime_state().host，
// move/merge 端口走 QOL_PORT_MOVE_MERGE 端口绑定，投影回调走本插件注册表。
namespace qol::features::extension_bag::merge {

// 资源 id 同时用于 claim 声明、InFlight 标记与 resource-ready 分发。
inline constexpr const char* kMergeResource = "ptrslot:MOVE_MERGE_GOT";

// ptrslot wrapper 入口；EB claims 表以该函数地址作为 handler。
uint64_t ui_equip_inven_item_proc_wrapper(void* control, uint64_t event, void* x2, void* param);

// 该 feature 的声明式资源（ptrslot:MOVE_MERGE_GOT）；由 assembly/claims.cpp 拼入 EB 总表。
const contracts::ResourceClaim* merge_claims(uint32_t* out_count);

void detach();
void set_enabled(bool enabled);
bool enabled();
bool wrapper_ready();
void set_wrapper_ready(bool ready);
bool is_merge_resource(const char* resource_id);

}  // namespace qol::features::extension_bag::merge
