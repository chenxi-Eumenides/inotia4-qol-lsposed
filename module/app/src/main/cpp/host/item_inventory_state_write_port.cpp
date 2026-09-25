#include "host/item_inventory_state_write_port.h"

#include <cstring>

#include "contracts/extension_bag_port.h"
#include "contracts/game_cache.h"
#include "core/native/inventory_trade.h"
#include "game/ops/item_inventory_state_write.h"
#include "game/state/game_state.h"

namespace qol::host {
namespace {

QolResult trade_error(contracts::QolItemInventoryStateWriteResult* out, const char* message) {
    const size_t length = std::strlen(message);
    if (length >= sizeof(out->error_message)) {
        return contracts::error(kQolErrInternal, "item trade error too long", 25);
    }
    std::memcpy(out->error_message, message, length + 1);
    out->error_message_len = static_cast<uint32_t>(length);
    return contracts::error(kQolErrInternal, out->error_message, out->error_message_len);
}

const char* sell_error(inventory_trade::Status status) {
    switch (status) {
        case inventory_trade::Status::kNotInWorld: return "not in game";
        case inventory_trade::Status::kTaskBagExcluded: return "task bag excluded";
        case inventory_trade::Status::kEmptySlot: return "slot empty";
        case inventory_trade::Status::kNoNativeItem: return "extension item unavailable";
        case inventory_trade::Status::kStale: return "slot empty";
        case inventory_trade::Status::kSymbolsUnresolved: return "symbol not resolved";
        case inventory_trade::Status::kNoSell: return "item cannot be sold";
        case inventory_trade::Status::kPriceInvalid: return "invalid sell price";
        case inventory_trade::Status::kCreditFailed: return "sell money update failed";
        case inventory_trade::Status::kRemoveFailed: return "sell failed";
        case inventory_trade::Status::kOk: return "";
        default: return "sell failed";
    }
}

const char* discard_error(inventory_trade::Status status) {
    switch (status) {
        case inventory_trade::Status::kNotInWorld: return "not in game";
        case inventory_trade::Status::kTaskBagExcluded: return "task bag excluded";
        case inventory_trade::Status::kEmptySlot: return "slot empty";
        case inventory_trade::Status::kNoNativeItem: return "extension item unavailable";
        case inventory_trade::Status::kStale: return "slot empty";
        case inventory_trade::Status::kSymbolsUnresolved: return "symbol not resolved";
        case inventory_trade::Status::kRemoveFailed: return "discard failed";
        case inventory_trade::Status::kOk: return "";
        default: return "discard failed";
    }
}

QolResult use_item(int32_t bag, int32_t slot,
                   contracts::QolItemInventoryStateWriteResult* out) {
    if (out == nullptr) return contracts::error(kQolErrInvalidArgument, "invalid item result", 19);
    *out = {};
    if (!game_in_world()) {
        return contracts::error(kQolErrNotReady, "not in game", 11);
    }
    if (bag == 5) {
        return trade_error(out, "task bag excluded");
    }
    InventoryItemRef item_ref;
    if (!inventory_item_ref_at(bag, slot, &item_ref)) {
        return trade_error(out, "slot empty");
    }
    if (item_ref.kind == InventoryItemKind::kExtension) {
        const std::string result = extension_bag_use_item(bag, slot);
        constexpr char kOk[] = "{\"ok\":true}";
        if (result == kOk) return contracts::ok();
        constexpr char kErrorPrefix[] = "{\"ok\":false,\"error\":\"";
        if (result.size() >= sizeof(kErrorPrefix) - 1 + 2 &&
            result.compare(0, sizeof(kErrorPrefix) - 1, kErrorPrefix) == 0 &&
            result.compare(result.size() - 2, 2, "\"}") == 0) {
            const size_t begin = sizeof(kErrorPrefix) - 1;
            const size_t length = result.size() - begin - 2;
            if (length < sizeof(out->error_message)) {
                std::memcpy(out->error_message, result.data() + begin, length);
                out->error_message[length] = '\0';
                out->error_message_len = static_cast<uint32_t>(length);
                return contracts::error(kQolErrInternal, out->error_message,
                                        out->error_message_len);
            }
        }
        return contracts::error(kQolErrInternal, "extension item use failed", 25);
    }
    return qol::game::ops::use_item(bag, slot, out);
}

QolResult sell_item(int32_t bag, int32_t slot,
                    contracts::QolItemInventoryStateWriteResult* out) {
    if (out == nullptr) return contracts::error(kQolErrInvalidArgument, "invalid item result", 19);
    *out = {};
    InventoryItemRef expected;
    const bool found = inventory_item_ref_at(bag, slot, &expected);
    const inventory_trade::Result result = inventory_trade::sell(
        bag, slot, found ? expected.native_item : nullptr, "api");
    if (result.status != inventory_trade::Status::kOk) return trade_error(out, sell_error(result.status));
    out->value = result.price;
    frame_cache_force_refresh();
    return contracts::ok();
}

QolResult discard_item(int32_t bag, int32_t slot,
                       contracts::QolItemInventoryStateWriteResult* out) {
    if (out == nullptr) return contracts::error(kQolErrInvalidArgument, "invalid item result", 19);
    *out = {};
    InventoryItemRef expected;
    const bool found = inventory_item_ref_at(bag, slot, &expected);
    const inventory_trade::Status status = inventory_trade::discard(
        bag, slot, found ? expected.native_item : nullptr);
    if (status != inventory_trade::Status::kOk) return trade_error(out, discard_error(status));
    frame_cache_force_refresh();
    return contracts::ok();
}

QolResult shop_buy(int32_t slot, contracts::QolItemInventoryStateWriteResult* out) {
    return qol::game::ops::shop_buy(slot, out);
}

QolResult move_item(int32_t bag, int32_t slot, int32_t count, int32_t to_bag,
                    int32_t to_slot,
                    contracts::QolItemInventoryStateWriteResult* out) {
    return qol::game::ops::move_item(bag, slot, count, to_bag, to_slot, out);
}

const contracts::QolItemInventoryStateWritePort kPort{
    contracts::kQolPortItemInventoryStateWriteVersion,
    &use_item,
    &sell_item,
    &discard_item,
    &shop_buy,
    &move_item,
};

}  // namespace

const contracts::QolItemInventoryStateWritePort& item_inventory_state_write_port() {
    return kPort;
}

}  // namespace qol::host
