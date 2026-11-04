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

QolResult read_bag_size_ref(const contracts::QolBagRef* ref, int32_t* out_size) {
    if (ref == nullptr || out_size == nullptr) {
        return error_result(kQolErrInvalidArgument, "invalid argument");
    }
    return native_bag_capacity_ref(ref, out_size);
}

QolResult read_is_empty_bag_ref(const contracts::QolBagRef* ref, int32_t* out_empty) {
    if (ref == nullptr || out_empty == nullptr) {
        return error_result(kQolErrInvalidArgument, "invalid argument");
    }
    *out_empty = 1;
    for (int32_t slot = 0; slot < 16; ++slot) {
        int32_t occupied = 0;
        const QolResult result = native_slot_occupied(ref, slot, &occupied);
        if (!contracts::succeeded(result)) return result;
        if (occupied != 0) {
            *out_empty = 0;
            break;
        }
    }
    return contracts::ok();
}

QolResult read_empty_bag_slot(int32_t* out_slot) { return empty_bag_slot(out_slot); }

QolResult read_is_having_empty_slot(int32_t needed, int32_t include_task_bag,
                                    int32_t* out_value) {
    if (out_value == nullptr) {
        return error_result(kQolErrInvalidArgument, "invalid argument");
    }
    if (fn_is_having_empty_slot == nullptr) {
        return error_result(kQolErrNotReady, "not ready");
    }
    *out_value = fn_is_having_empty_slot(needed, include_task_bag);
    return contracts::ok();
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
      &read_cursor_item, &read_detail_item, &read_bag_size_ref, &read_is_empty_bag_ref,
      &read_empty_bag_slot, &read_is_having_empty_slot},
};

}  // namespace

const contracts::QolInventoryExtrasPort& inventory_extras_port() { return kPort; }

}  // namespace qol::game
