#pragma once

#include <cstdint>

namespace qol::features::extra_bag::model {

// 普通物品数据；入库/出库不校验 item_id 或 count。
struct ItemData {
    uint32_t item_id;
    uint32_t count;
};

}  // namespace qol::features::extra_bag::model
