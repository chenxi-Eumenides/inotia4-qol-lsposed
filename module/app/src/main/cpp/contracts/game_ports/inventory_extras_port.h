#pragma once

#include "../game_ports/item_port.h"
#include "../result.h"
#include "../version.h"

namespace qol::contracts {

struct QolInventoryExtrasReadApi {
    QolResult (*for_each_bag_slot)(QolInventoryBagSlotFn callback, void* ctx);
    QolResult (*bag_size)(int32_t bag, int32_t* out_size);
    QolResult (*is_empty_bag)(int32_t bag, int32_t* out_empty);
    QolResult (*find_save_slot)(const void* item, int32_t* out_slot);
    QolResult (*cursor_item)(const void* control, void** out_item);
    QolResult (*detail_item)(void** out_item);
    // 1.1：空间化袋尺寸查询。原生空间按 bag 索引；注册源空间由源自身解析。
    QolResult (*bag_size_ref)(const QolBagRef* ref, int32_t* out_size);
    QolResult (*is_empty_bag_ref)(const QolBagRef* ref, int32_t* out_empty);
    // First empty native bag slot 0..4, or -1 when all are occupied.
    QolResult (*empty_bag_slot)(int32_t* out_slot);
};

struct QolInventoryExtrasPort {
    QolVersion abi_version;
    QolInventoryExtrasReadApi read;
};

}  // namespace qol::contracts
