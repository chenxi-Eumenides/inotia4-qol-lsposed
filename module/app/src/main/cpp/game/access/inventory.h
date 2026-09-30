#pragma once

#include "contracts/game_ports/item_port.h"
#include "contracts/game_host.h"

// 游戏层：库存只读遍历与引用适配（ARCHITECTURE.md §5）。
// 物理袋/扩展袋的底层布局和扩展背包端口均留在 game/ 与既有宿主实现内；插件只接收
// contracts 中的不透明库存引用。

namespace qol::game {

// 按袋号和槽号读取库存引用；空槽返回 kQolErrNotFound，底层尚未就绪返回
// kQolErrNotReady。
QolResult inventory_item_ref_at(int32_t bag, int32_t slot,
                               contracts::QolInventoryItemRef* out);

// 遍历原版物理袋 0..5；回调返回 1 时停止。回调期间引用有效，返回后不得保存。
QolResult for_each_bag_slot(contracts::QolInventoryBagSlotFn callback, void* ctx);

// 读取物理袋对象；包含任务袋 5，返回的不透明对象只在当前游戏线程调用窗内使用。
QolResult physical_item_at(int32_t bag, int32_t slot, void** out_item);

// 读取原版装备详情的当前描述类型、袋号和控件句柄。
QolResult detail_context(int32_t* out_desc_type, int32_t* out_bag, void** out_control);
QolResult cursor_index(const void* control, int32_t* out_slot);

// 原版袋容量与保存槽查询；均为只读原语。
QolResult bag_size(int32_t bag, int32_t* out_size);
QolResult is_empty_bag(int32_t bag, int32_t* out_empty);
QolResult find_save_slot(const void* item, int32_t* out_slot);
QolResult current_bag(int32_t* out_bag);
QolResult set_current_bag(int32_t bag);
QolResult desc_type(int32_t* out_type);
QolResult set_desc_type(int32_t type);
QolResult bag_capacity_word(int32_t bag, uint32_t* out_word);
QolResult set_bag_capacity_word(int32_t bag, uint32_t word);
QolResult set_physical_item(int32_t bag, int32_t slot, void* item);

// Hook dispatch belongs to Game Adapter so an ABI object pointer is never returned to a plugin.
void* game_inventory_find_item_hook(int32_t category);
QolResult game_inventory_bind_find_item_backup(contracts::QolFindItemAbiFn backup);
QolResult game_original_find_item_slot(contracts::QolItemHandle item, int32_t* out_bag,
                                       int32_t* out_slot);
QolResult game_item_identity_at(int32_t bag, int32_t slot, contracts::QolItemHandle* out_item);
void game_begin_original_only();
void game_end_original_only();
QolResult game_native_item_at(int32_t bag, int32_t slot, void** out_item);
QolResult game_native_item_for_handle(contracts::QolItemHandle item, void** out_item);

}  // namespace qol::game
