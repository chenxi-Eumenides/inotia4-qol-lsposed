#pragma once

// core.inventory 的存储核心：槽表注册 + 查询/移动/取放/部分移除。
// 主线程单线程模型；所有写操作先完成预检，再写目标槽，最后清源槽。

#include <cstdint>
#include <memory>
#include <vector>

#include "contracts/inventory_access.h"
#include "plugins/core.inventory/store/slot_table.h"

namespace qol::features::core_inventory::store {

class InventoryStore {
public:
    InventoryStore() = default;
    InventoryStore(const InventoryStore&) = delete;
    InventoryStore& operator=(const InventoryStore&) = delete;

    // 注册/注销只在主线程、无进行中操作时调用。重复 space_id 或空表返回 false。
    bool add_table(std::unique_ptr<SlotTable> table);
    void clear();

    SlotTable* find_table(int32_t space_id) const;

    QolResult item_at(const contracts::QolInventorySlotRef& ref,
                      contracts::QolInventoryItemRef* out) const;
    QolResult locate(contracts::QolItemHandle item,
                     contracts::QolInventoryItemRef* out) const;
    QolResult find_first(int32_t category, contracts::QolInventoryItemRef* out) const;
    QolResult find_empty(int32_t space_id, contracts::QolInventorySlotRef* out) const;
    QolResult count_item(int32_t category, int32_t* out_count) const;

    QolResult move(const contracts::QolInventorySlotRef& from,
                   const contracts::QolInventorySlotRef& to);
    QolResult take(const contracts::QolInventorySlotRef& ref,
                   contracts::QolInventoryItem* out);
    QolResult place(const contracts::QolInventorySlotRef& ref,
                    const contracts::QolInventoryItem& item);
    QolResult remove_count(const contracts::QolInventorySlotRef& ref, int32_t count);

private:
    std::vector<std::unique_ptr<SlotTable>> tables_;
};

}  // namespace qol::features::core_inventory::store
