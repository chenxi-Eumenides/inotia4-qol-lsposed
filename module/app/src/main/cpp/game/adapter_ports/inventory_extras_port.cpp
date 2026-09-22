#include "game/adapter_ports/inventory_extras_port.h"

#include <cstring>

#include "contracts/game_ports/ports.h"
#include "game/access/game_access.h"
#include "game/access/inventory.h"

namespace qol::game {
namespace {

using QolResult = ::QolResult;

QolResult error_result(int32_t code, const char* message) {
    return contracts::error(code, message, static_cast<uint32_t>(std::strlen(message)));
}

QolResult read_for_each_bag_slot(contracts::QolInventoryBagSlotFn callback, void* ctx) {
    if (callback == nullptr) return error_result(kQolErrInvalidArgument, "invalid argument");
    return for_each_bag_slot(callback, ctx);
}

QolResult read_bag_size(int32_t bag, int32_t* out_size) { return bag_size(bag, out_size); }

QolResult read_is_empty_bag(int32_t bag, int32_t* out_empty) {
    return is_empty_bag(bag, out_empty);
}

QolResult read_find_save_slot(const void* item, int32_t* out_slot) {
    return find_save_slot(item, out_slot);
}

QolResult read_cursor_item(const void* control, void** out_item) {
    if (control == nullptr || out_item == nullptr) {
        return error_result(kQolErrInvalidArgument, "invalid argument");
    }
    if (fn_control_object_get_cursor == nullptr) {
        return error_result(kQolErrNotReady, "not ready");
    }
    *out_item = fn_control_object_get_cursor(const_cast<void*>(control));
    return *out_item == nullptr ? error_result(kQolErrNotReady, "not ready") : contracts::ok();
}

QolResult read_detail_item(void** out_item) {
    if (out_item == nullptr) return error_result(kQolErrInvalidArgument, "invalid argument");
    if (fn_ui_desc_get_data == nullptr) return error_result(kQolErrNotReady, "not ready");
    *out_item = fn_ui_desc_get_data();
    return *out_item == nullptr ? error_result(kQolErrNotFound, "detail item not found")
                                : contracts::ok();
}

const contracts::QolInventoryExtrasPort kPort{
    contracts::kQolPortInventoryExtrasVersion,
    {&read_for_each_bag_slot, &read_bag_size, &read_is_empty_bag, &read_find_save_slot,
     &read_cursor_item, &read_detail_item},
};

}  // namespace

const contracts::QolInventoryExtrasPort& inventory_extras_port() { return kPort; }

}  // namespace qol::game
