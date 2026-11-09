#include "api.h"

#include <cstring>

#include "plugins/core.inventory/transfer/move_item.h"
#include "plugins/core.inventory/transfer/native_transport.h"
#include "plugins/core.inventory/transfer/replace_item.h"
#include "plugins/core.inventory/transfer/source_transport.h"
#if !defined(QOL_CORE_INVENTORY_TRANSFER_SEPARATE)
// 宿主测试直接编译 plugin.cpp，不读取插件 CMake 源清单；插件 target 单独编译该实现。
#include "plugins/core.inventory/transfer/move_item.cpp"
#include "plugins/core.inventory/transfer/replace_item.cpp"
#endif

namespace qol::features::core_inventory::transfer {
namespace {

QolResult invalid_argument(const char* message) {
    return contracts::error(kQolErrInvalidArgument, message,
                            static_cast<uint32_t>(std::strlen(message)));
}

bool valid_handle(contracts::QolItemHandle item) {
    return item.slot != 0 && item.generation != 0;
}

bool is_native_space(int32_t space) {
    return space == contracts::kQolBagSpaceOriginal ||
           space == contracts::kQolBagSpaceVariant;
}

QolResult api_move_item(const contracts::QolBagRef* from, int32_t from_slot, int32_t count,
                        const contracts::QolBagRef* to, int32_t to_slot,
                        contracts::QolBagMutationResult* out_target) {
    if (from == nullptr || to == nullptr) {
        return invalid_argument("invalid inventory transfer request");
    }
    return move_item::move_item(*from, from_slot, count, *to, to_slot, out_target);
}

QolResult api_serialize_item(contracts::QolItemHandle item, int32_t count,
                             contracts::QolBagTransfer* out) {
    if (out == nullptr || !valid_handle(item) || count <= 0) {
        return invalid_argument("invalid inventory transfer request");
    }
    return native_transport::export_detached_item(item, count, out);
}

QolResult api_replace_item(contracts::QolItemHandle expected,
                           const contracts::QolBagTransfer* replacement,
                           contracts::QolBagMutationResult* out) {
    return replace_item::replace_item(expected, replacement, out);
}

QolResult api_locate(int32_t space, contracts::QolItemHandle item, contracts::QolBagRef* out,
                     int32_t* out_slot) {
    if (!valid_handle(item)) {
        return invalid_argument("invalid inventory transfer request");
    }
    // 按 space 分派：原生 space 走 item port 袋扫描，注册源走 bag source identify。
    if (is_native_space(space)) {
        return native_transport::locate(space, item, out, out_slot);
    }
    if (space >= contracts::kQolBagSpaceFirstRegistered) {
        return source_transport::locate(space, item, out, out_slot);
    }
    return invalid_argument("invalid inventory transfer request");
}

}  // namespace

const contracts::QolInventoryTransferApi& inventory_transfer_api() {
    static const contracts::QolInventoryTransferApi api = {
        sizeof(contracts::QolInventoryTransferApi),
        &api_move_item,
        &api_serialize_item,
        &api_locate,
        &api_replace_item,
    };
    return api;
}

}  // namespace qol::features::core_inventory::transfer
