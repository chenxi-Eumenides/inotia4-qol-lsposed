#pragma once

#include "contracts/game_ports/item_inventory_state_write_port.h"

namespace qol::game::ops {

QolResult use_item(int32_t bag, int32_t slot,
                   contracts::QolItemInventoryStateWriteResult* out);
QolResult shop_buy(int32_t slot, contracts::QolItemInventoryStateWriteResult* out);
QolResult move_item(int32_t bag, int32_t slot, int32_t count, int32_t to_bag,
                    int32_t to_slot,
                    contracts::QolItemInventoryStateWriteResult* out);
QolResult add_item(int32_t category, int32_t count, int32_t socket_total,
                   int32_t enhance_remaining, int32_t rarity_group,
                   const contracts::QolItemCountApi* item_count,
                   contracts::QolItemInventoryStateWriteResult* out);
QolResult load_item(const uint8_t* payload, uint32_t size, contracts::QolItemHandle* out_item);
QolResult free_loaded_item(contracts::QolItemHandle item);
// v2.3 detached 创建：返回调用方拥有的句柄（owner 与 load_item 相同），不落位。
QolResult create_item(int32_t category, int32_t arg1, int32_t arg2, int32_t arg3,
                      contracts::QolItemHandle* out_item);
QolResult create_perfect_item(int32_t category, contracts::QolItemHandle* out_item);
// v2.4 自动落位：成功转 game-owned；失败保持 detached 且不释放对象。
QolResult save_detached_item(contracts::QolItemHandle item, int32_t* out_saved);
QolResult place_item(const contracts::QolBagRef* ref, int32_t slot,
                     contracts::QolItemHandle item,
                     contracts::QolItemInventoryStateWriteResult* out);
QolResult remove_item(contracts::QolItemHandle item, int32_t count,
                      contracts::QolItemInventoryStateWriteResult* out);
QolResult move_handle(contracts::QolItemHandle item, int32_t count,
                      const contracts::QolBagRef* ref, int32_t slot,
                      contracts::QolItemInventoryStateWriteResult* out);
QolResult equip_item(int32_t role, int32_t bag, int32_t slot,
                     contracts::QolItemInventoryStateWriteResult* out);
QolResult unequip_item(int32_t role, int32_t equip_slot,
                       contracts::QolItemInventoryStateWriteResult* out);
QolResult put_jewel(int32_t role, int32_t bag, int32_t slot, int32_t equip_slot,
                    contracts::QolItemInventoryStateWriteResult* out);
QolResult enchant(int32_t role, int32_t bag, int32_t slot, int32_t equip_slot,
                  contracts::QolItemInventoryStateWriteResult* out);
QolResult equip_handle(int32_t role, contracts::QolItemHandle item, int32_t equip_slot,
                       contracts::QolItemInventoryStateWriteResult* out);
QolResult unequip_handle(int32_t role, int32_t equip_slot,
                         contracts::QolItemInventoryStateWriteResult* out);
QolResult put_jewel_handles(int32_t role, contracts::QolItemHandle equip_item,
                            contracts::QolItemHandle jewel_item,
                            contracts::QolItemInventoryStateWriteResult* out);
// 按 category 删除单件（原版袋经 fn_remove_item 指针路径；扩展袋经中立 provider）。
QolResult remove_item(int32_t category, contracts::QolItemInventoryStateWriteResult* out);
// v2.5 payload 字段写入。
QolResult apply_payload_mutation(contracts::QolItemHandle item,
                                 const contracts::QolItemPayloadMutation* mutation);
QolResult apply_payload_to_item(contracts::QolItemHandle item, const uint8_t* payload,
                                uint32_t payload_size);

}  // namespace qol::game::ops
