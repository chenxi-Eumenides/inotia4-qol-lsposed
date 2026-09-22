#include "game/adapter_ports/item_port.h"

#include <cstring>

#include "contracts/game_ports/ports.h"
#include "game/access/attr_range_ui.h"
#include "game/access/game_access.h"
#include "game/access/inventory.h"
#include "game/access/item_class.h"

namespace qol::game {
namespace {

using QolResult = ::QolResult;

QolResult error_result(int32_t code, const char* message) {
    return contracts::error(code, message, static_cast<uint32_t>(std::strlen(message)));
}

QolResult not_ready() { return error_result(kQolErrNotReady, "not ready"); }

QolResult read_category(const void* item, int32_t* out) {
    if (item == nullptr || out == nullptr) {
        return error_result(kQolErrInvalidArgument, "invalid argument");
    }
    const int value = attr_item_category(item);
    if (value < 0) return not_ready();
    *out = value;
    return contracts::ok();
}

QolResult read_real_equip(const void* item, int32_t* out) {
    if (item == nullptr || out == nullptr) {
        return error_result(kQolErrInvalidArgument, "invalid argument");
    }
    if (!item_equip_primitives_ready()) return not_ready();
    *out = item_is_real_equip(item) ? 1 : 0;
    return contracts::ok();
}

QolResult read_jewel_category(int32_t category, int32_t* out) {
    if (category < 0 || out == nullptr) {
        return error_result(kQolErrInvalidArgument, "invalid argument");
    }
    *out = attr_is_jewel_category(category) ? 1 : 0;
    return contracts::ok();
}

QolResult read_no_sell(int32_t category, int32_t* out) {
    if (category < 0 || out == nullptr) {
        return error_result(kQolErrInvalidArgument, "invalid argument");
    }
    if (fn_item_is_no_sell == nullptr) return not_ready();
    *out = fn_item_is_no_sell(category) != 0 ? 1 : 0;
    return contracts::ok();
}

QolResult read_sell_price(const void* item, int64_t* out) {
    if (item == nullptr || out == nullptr) {
        return error_result(kQolErrInvalidArgument, "invalid argument");
    }
    const int value = item_sell_price(item);
    if (value < 0) return not_ready();
    *out = value;
    return contracts::ok();
}

QolResult read_cumulate_count(const void* item, int32_t* out) {
    if (item == nullptr || out == nullptr) {
        return error_result(kQolErrInvalidArgument, "invalid argument");
    }
    if (fn_get_cumulate_count == nullptr) return not_ready();
    *out = fn_get_cumulate_count(const_cast<void*>(item));
    return contracts::ok();
}

QolResult read_item_ref_at(int32_t bag, int32_t slot, contracts::QolInventoryItemRef* out) {
    if (out == nullptr || bag < 0 || bag > 10 || slot < 0 || slot >= 16) {
        return error_result(kQolErrInvalidArgument, "invalid argument");
    }
    return inventory_item_ref_at(bag, slot, out);
}

QolResult read_physical_item_at(int32_t bag, int32_t slot, void** out) {
    return physical_item_at(bag, slot, out);
}

QolResult read_detail_context(int32_t* out_desc_type, int32_t* out_bag, void** out_control) {
    return detail_context(out_desc_type, out_bag, out_control);
}

QolResult read_cursor_index(const void* control, int32_t* out_slot) {
    return cursor_index(control, out_slot);
}

const contracts::QolItemPort kItemPort{
    contracts::kQolPortItemVersion,
    {&read_category, &read_real_equip, &read_jewel_category, &read_no_sell, &read_sell_price,
     &read_cumulate_count, &read_item_ref_at, &read_physical_item_at, &read_detail_context,
     &read_cursor_index},
};

}  // namespace

const contracts::QolItemPort& item_port() { return kItemPort; }

}  // namespace qol::game
