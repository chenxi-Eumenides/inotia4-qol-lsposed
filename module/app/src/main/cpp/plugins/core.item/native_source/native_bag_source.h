#pragma once

// 原版袋源骨架：实现 bags::BagSourceIo，内部操作委托 Host 端口。
// 实现待补；当前未加入 CMake，不参与构建。

#include <cstdint>

#include "contracts/game_ports/item_inventory_state_write_port.h"
#include "contracts/game_ports/item_port.h"
#include "plugins/core.item/bags/bag_source_io.h"

namespace qol::features::core_item::native_source {

class NativeBagSource final : public bags::BagSourceIo {
public:
    NativeBagSource(int32_t space_id, const contracts::QolItemPort* read_port,
                    const contracts::QolItemInventoryStateWritePort* write_port);
    ~NativeBagSource() override = default;

    int32_t space_id() const override;
    int32_t count() const override;
    QolResult info(int32_t bag, contracts::QolBagInfo* out) const override;
    QolResult item_at(int32_t bag, int32_t slot,
                      contracts::QolInventoryItemRef* out) const override;
    QolResult for_each_item(contracts::QolInventoryBagSlotFn fn, void* ctx) const override;

    // 原版袋内部移动由原版代码完成；本实现只转发 Host move_item。
    QolResult move_within(const contracts::QolBagRef& from, int32_t from_slot, int32_t count,
                          const contracts::QolBagRef& to, int32_t to_slot,
                          contracts::QolBagMutationResult* out) override;

    QolResult export_item(contracts::QolItemHandle item, int32_t count,
                          contracts::QolBagTransfer* out) const override;
    QolResult import_item(const contracts::QolBagTransfer& transfer, int32_t preferred_bag,
                          int32_t preferred_slot,
                          contracts::QolBagMutationResult* out) override;
    QolResult remove(contracts::QolItemHandle item, int32_t count,
                     contracts::QolBagMutationResult* out) override;
    QolResult identify(contracts::QolItemHandle item, contracts::QolBagRef* out,
                       int32_t* out_slot) const override;

private:
    int32_t space_id_;
    const contracts::QolItemPort* read_port_;
    const contracts::QolItemInventoryStateWritePort* write_port_;
};

}  // namespace qol::features::core_item::native_source
