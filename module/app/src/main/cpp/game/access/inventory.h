#pragma once

#include "contracts/game_ports/item_port.h"
#include "contracts/game_host.h"

// 游戏层：原版与 C18 原生库存只读遍历、引用适配（ARCHITECTURE.md §5）。
// 扩展袋数据通过 core.item 注册的 QolBagSource 访问；插件只接收 contracts 中的不透明库存引用。

namespace qol::game {

// 按袋号和槽号读取库存引用；空槽返回 kQolErrNotFound，底层尚未就绪返回
// kQolErrNotReady。
QolResult inventory_item_ref_at(int32_t bag, int32_t slot,
                               contracts::QolInventoryItemRef* out);

// 遍历原版物理袋 0..5；回调返回 1 时停止。回调期间引用有效，返回后不得保存。
QolResult for_each_bag_slot(contracts::QolInventoryBagSlotFn callback, void* ctx);

// 读取物理袋对象；包含任务袋 5，返回的不透明对象只在当前游戏线程调用窗内使用。
QolResult physical_item_at(int32_t bag, int32_t slot, void** out_item);
QolResult detail_item_context(contracts::QolItemDetailContext* out_context);
// 读取快捷栏条目的物品类别（仅 type=2 的物品快捷栏）；用于把扩展袋消耗品
// 接入 CHAR_ProcessShortcut，不读取/写入 INVEN_pItem。
bool shortcut_item_category(int32_t shortcut, int32_t* out_category);

// Game-private UI accessors. Native control pointers never cross adapter-port boundaries.
QolResult detail_context(int32_t* out_desc_type, int32_t* out_bag, void** out_control);
QolResult cursor_index(const void* control, int32_t* out_slot);

// 原版袋容量与保存槽查询；均为只读原语。
QolResult bag_size(int32_t bag, int32_t* out_size);
QolResult is_empty_bag(int32_t bag, int32_t* out_empty);
QolResult find_save_slot(const void* item, int32_t* out_slot);
QolResult current_bag(int32_t* out_bag);
QolResult set_current_bag(int32_t bag);

// 原生袋槽布局：原版为 INVEN_pItem 的 6 袋×0x80；C18 改版客户端把袋对象表和
// 槽表搬到独立 bss 数组（容量 19）。布局按运行时探测，不写死版本号。
struct NativeBagLayout {
    uint8_t* slots;  // 袋槽表基址（每袋 0x80 字节、每槽 8 字节物品指针）
    int bag_limit;   // 可用袋号上界（不含）
};
NativeBagLayout native_bag_layout();
void invalidate_native_bag_layout_cache();
QolResult inventory_bag_count(int32_t space_id, int32_t* out_count);
QolResult inventory_bag_info(const contracts::QolBagRef* ref,
                             contracts::QolBagInfo* out_info);
QolResult inventory_space_item_at(const contracts::QolBagRef* ref, int32_t slot,
                                  contracts::QolInventoryItemRef* out);
QolResult inventory_space_item_pointer(const contracts::QolBagRef* ref, int32_t slot,
                                       void** out);
QolResult inventory_for_each_space_slot(int32_t space_id,
                                        contracts::QolInventoryBagSlotFn callback, void* ctx);
QolResult desc_type(int32_t* out_type);
QolResult set_desc_type(int32_t type);
QolResult bag_capacity_word(int32_t bag, uint32_t* out_word);
QolResult set_bag_capacity_word(int32_t bag, uint32_t word);
QolResult set_physical_item(int32_t bag, int32_t slot, void* item);
QolResult set_native_space_item(const contracts::QolBagRef* ref, int32_t slot, void* item);
// 交换两个已占用槽的对象指针；物品对象本身不重建，句柄保持有效。
QolResult swap_native_space_items(const contracts::QolBagRef* a, int32_t slot_a,
                                  const contracts::QolBagRef* b, int32_t slot_b);
QolResult serialize_item(contracts::QolItemHandle item, uint8_t* out, uint32_t capacity,
                         uint32_t* out_size);

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
