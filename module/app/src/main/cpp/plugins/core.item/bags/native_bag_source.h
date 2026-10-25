#pragma once

#include "contracts/game_ports/item_port.h"

namespace qol::features::core_item::bags {

class NativeBagSource {
public:
    explicit NativeBagSource(const contracts::QolItemPort* port);

    int32_t count(int32_t space_id) const;
    QolResult info(const contracts::QolBagRef& bag, contracts::QolBagInfo* out) const;
    QolResult item_at(const contracts::QolBagRef& bag, int32_t slot,
                      contracts::QolInventoryItemRef* out) const;
    QolResult for_each(int32_t space_id, contracts::QolInventoryBagSlotFn fn,
                       void* ctx) const;

private:
    bool supports_space(int32_t space_id) const;

    const contracts::QolItemPort* port_;
};

}  // namespace qol::features::core_item::bags
