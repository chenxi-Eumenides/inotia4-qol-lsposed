#pragma once

// 槽表最小接口。生产实现（原生表/外部表）后续接入；MemorySlotTable 只用于
// core.inventory 的 host 测试和契约验证，不进入生产装配。

#include <cstddef>
#include <cstdint>
#include <vector>

#include "contracts/inventory_access.h"

namespace qol::features::core_inventory::store {

class SlotTable {
public:
    virtual ~SlotTable() = default;

    virtual int32_t space_id() const = 0;
    virtual int32_t bag_count() const = 0;
    virtual int32_t capacity(int32_t bag) const = 0;

    // 合法参数下 get 返回 false 表示空槽；set/clear 返回 false 表示参数非法。
    virtual bool get(int32_t bag, int32_t slot, contracts::QolInventoryItem* out) const = 0;
    virtual bool set(int32_t bag, int32_t slot, const contracts::QolInventoryItem& item) = 0;
    virtual bool clear(int32_t bag, int32_t slot) = 0;
};

class MemorySlotTable final : public SlotTable {
public:
    MemorySlotTable(int32_t space_id, std::vector<int32_t> capacities);

    int32_t space_id() const override;
    int32_t bag_count() const override;
    int32_t capacity(int32_t bag) const override;

    bool get(int32_t bag, int32_t slot, contracts::QolInventoryItem* out) const override;
    bool set(int32_t bag, int32_t slot, const contracts::QolInventoryItem& item) override;
    bool clear(int32_t bag, int32_t slot) override;

private:
    struct Slot {
        bool occupied = false;
        contracts::QolInventoryItem item{};
    };

    bool locate(int32_t bag, int32_t slot, size_t* out_index) const;

    int32_t space_id_;
    std::vector<int32_t> capacities_;
    std::vector<Slot> slots_;
};

}  // namespace qol::features::core_inventory::store
