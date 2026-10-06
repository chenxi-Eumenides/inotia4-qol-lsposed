#include "game/adapter_ports/move_merge_port.h"

#include <cstring>

#include "game/access/game_access.h"
#include "game/access/inventory.h"
#include "game/access/item_handle_registry.h"
#include "game/adapter_ports/game_host_port.h"

namespace qol::game {
namespace {

QolResult invalid_argument() {
    constexpr const char message[] = "invalid argument";
    return contracts::error(kQolErrInvalidArgument, message, sizeof(message) - 1);
}

QolResult wrong_thread() {
    constexpr const char message[] = "wrong thread";
    return contracts::error(kQolErrBlocked, message, sizeof(message) - 1);
}

QolResult not_ready() {
    constexpr const char message[] = "not ready";
    return contracts::error(kQolErrNotReady, message, sizeof(message) - 1);
}

QolResult move_item(contracts::QolItemHandle source, int32_t count, int32_t target_bag,
                    int32_t target_slot, int32_t* out_result) {
    if (!game_host_is_main_thread()) return wrong_thread();
    if (out_result == nullptr || count <= 0 || target_bag < 0 || target_bag > 5 ||
        target_slot < 0 || target_slot > 15) {
        return invalid_argument();
    }
    const ItemHandleResolution item = item_handle_registry().resolve_item(
        source, contracts::kQolItemHandleOwnerGame);
    if (item.error != ItemHandleError::kNone || item.item == nullptr) return invalid_argument();
    if (fn_inven_move_item == nullptr) return not_ready();

    *out_result = fn_inven_move_item(item.item, count, target_bag, target_slot);
    return contracts::ok();
}

QolResult validate_native_item(contracts::QolItemHandle handle) {
    if (!game_host_is_main_thread()) return wrong_thread();
    if (handle.slot == 0 || handle.generation == 0) return invalid_argument();
    const ItemHandleResolution item = item_handle_registry().resolve_item(
        handle, contracts::kQolItemHandleOwnerGame);
    if (item.error == ItemHandleError::kWrongThread) return wrong_thread();
    if (item.error != ItemHandleError::kNone || item.item == nullptr) return invalid_argument();
    return contracts::ok();
}

QolResult current_bag(int32_t* out_bag) {
    if (!game_host_is_main_thread()) return wrong_thread();
    if (out_bag == nullptr) return invalid_argument();

    int32_t bag = -1;
    const QolResult result = qol::game::current_bag(&bag);
    if (!contracts::succeeded(result)) return result;
    if (bag < 0 || bag > 5) return invalid_argument();

    *out_bag = bag;
    return contracts::ok();
}

const contracts::QolMoveMergePort kMoveMergePort{
    contracts::kQolMoveMergePortVersion,
    &move_item,
    &current_bag,
    &validate_native_item,
};

}  // namespace

const contracts::QolMoveMergePort& move_merge_port() { return kMoveMergePort; }

}  // namespace qol::game
