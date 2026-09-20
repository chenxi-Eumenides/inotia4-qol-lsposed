#pragma once

#include <cstdint>

// 游戏层：ITEMCLASS/ITEMDATABASE 记录访问与装备判定链物品侧原语（ARCHITECTURE.md §5）。
// GOT/VMA 与游戏函数指针只在本层出现；features 经语义函数读取，不接触 game_symbols.h。
//
// 线程纪律：以下函数均为纯读。item_class_record 读游戏静态表，wrapper 在游戏线程与 HTTP
// 线程（data_op_equip 经 fn_can_equip 进入 CanEquipItem wrapper）均可安全调用。

namespace qol::game {

// ITEMCLASS 表记录（category 为 item_class_category() 解出的类别）。
// 双层 GOT 解引用 + 记录长度校验；g_base 未就绪、表基址/记录长度为 0 → nullptr。
void* item_class_record(uint16_t category);

// 记录 +7 的「NPC 专属保护」bit4；record 为 nullptr → false。
bool item_class_no_equip_bit(const void* record);

// 物品类别 = item+I_TYPE bits6-15；item 为空返回 -1。
int item_class_category(const void* item);

// 物品只读位域原语；item 为空返回 -1。
int item_enchant_remaining(const void* item);
int item_socket_filled(const void* item);
int item_socket_total(const void* item);

// 独立宝石 item+I_COUNT 的属性值 bits0-10；item 为空返回 -1。
int item_jewel_value(const void* item);
// 独立宝石 item+I_COUNT 的属性类型 bits18-23；item 为空返回 -1。
int item_jewel_attribute_type(const void* item);

// 读取游戏提供的基础出售单价；未解析或 item 为空返回 -1。
int item_sell_price(const void* item);

// ITEMCLASSBASE 记录 +2 == 0x1f 的背包类别判定；表未就绪返回 false。
bool item_class_table_ready();
bool item_is_backpack_category(int category);

// 装备详情菜单角色类型（UIEquip desc_type）；未就绪 → -1。
int equip_desc_role_type();

// 装备判定链重放所需的物品侧原语是否都已解析（ITEM_IsRealEquip / ITEM_GetEquipLevel）。
// false = 调用方必须 fail-closed（保持原版拒绝），不得调用下面两个原语。
bool item_equip_primitives_ready();
bool item_is_real_equip_resolved();
bool item_equip_level_resolved();

// 以下原语只在 item_equip_primitives_ready() 为真时调用；未解析返回安全缺省。
bool item_is_real_equip(const void* item);
int item_equip_level(const void* item);

}  // namespace qol::game
