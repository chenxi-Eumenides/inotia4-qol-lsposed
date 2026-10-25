#include "item_action.h"

#include "move_item.h"
#include "sell_item.h"

#include "plugins/core.item/runtime/context.h"

namespace qol::features::core_item {
const contracts::QolBagSource* find_bag_source(int32_t space_id);
void request_inventory_refresh();
}

namespace qol::features::core_item {
namespace {

using contracts::InventoryActionStatus;
using contracts::QolActionRef;
using contracts::QolBagMutationResult;
using contracts::QolInventoryItemRef;
using contracts::QolItemHandle;

bool valid_ref(const QolActionRef& ref) {
    return ref.space_id >= contracts::kQolBagSpaceOriginal &&
           ref.space_id <= contracts::kQolBagSpaceFirstRegistered && ref.bag >= 0 &&
           ref.slot >= 0;
}

bool native_space(int32_t space_id) {
    return space_id == contracts::kQolBagSpaceOriginal ||
           space_id == contracts::kQolBagSpaceC18;
}

bool valid_handle(QolItemHandle item) {
    return item.slot != 0 && item.generation != 0;
}

int32_t native_bag_number(const QolActionRef& ref) {
    return ref.space_id == contracts::kQolBagSpaceOriginal
               ? ref.bag
               : contracts::kQolOriginalBagCount + ref.bag;
}

InventoryActionStatus status_from_result(const QolResult& result, bool item_was_resolved) {
    switch (result.code) {
        case kQolErrInvalidArgument:
            return InventoryActionStatus::kInvalidRef;
        case kQolErrNotFound:
            return item_was_resolved ? InventoryActionStatus::kStale
                                     : InventoryActionStatus::kEmpty;
        case kQolErrConflict:
            return InventoryActionStatus::kConflict;
        case kQolErrBlocked:
            return InventoryActionStatus::kFull;
        case kQolErrUnsupported:
            return InventoryActionStatus::kUnsupported;
        default:
            return InventoryActionStatus::kInternal;
    }
}

InventoryActionStatus validate_world() {
    if (g_port == nullptr || g_port->read.in_world == nullptr) {
        return InventoryActionStatus::kInternal;
    }
    int32_t in_world = 0;
    const QolResult result = g_port->read.in_world(&in_world);
    if (!contracts::succeeded(result)) return status_from_result(result, false);
    return in_world != 0 ? InventoryActionStatus::kOk : InventoryActionStatus::kNotInWorld;
}

QolResult read_item(const QolActionRef& ref, QolInventoryItemRef* out) {
    if (out == nullptr || g_port == nullptr ||
        !contracts::qol_item_port_bag_space_size_valid(g_port) ||
        g_port->read.item_at == nullptr) {
        constexpr char kMessage[] = "inventory action read unavailable";
        return contracts::error(kQolErrNotReady, kMessage,
                                static_cast<uint32_t>(sizeof(kMessage) - 1));
    }
    const contracts::QolBagRef bag{ref.space_id, ref.bag};
    return g_port->read.item_at(&bag, ref.slot, out);
}

InventoryActionStatus resolve_item(const QolActionRef& ref, QolInventoryItemRef* out) {
    const QolResult result = read_item(ref, out);
    if (!contracts::succeeded(result)) return status_from_result(result, false);
    if (out->space_id != ref.space_id || out->bag != ref.bag || out->slot != ref.slot ||
        !valid_handle(out->item) || out->category <= 0) {
        return InventoryActionStatus::kStale;
    }
    if (native_space(ref.space_id)) {
        if (out->kind != static_cast<int32_t>(contracts::QolInventoryItemKind::kOriginal)) {
            return InventoryActionStatus::kStale;
        }
    } else if (out->kind != static_cast<int32_t>(contracts::QolInventoryItemKind::kProvider)) {
        return InventoryActionStatus::kStale;
    }
    return InventoryActionStatus::kOk;
}

InventoryActionStatus validate_action_ref(const QolActionRef& ref) {
    if (!valid_ref(ref)) return InventoryActionStatus::kInvalidRef;
    if (ref.space_id == contracts::kQolBagSpaceOriginal &&
        ref.bag == contracts::kQolTaskBagIndex) {
        return InventoryActionStatus::kTaskBagExcluded;
    }
    return InventoryActionStatus::kOk;
}

bool target_occupied(const QolActionRef& ref) {
    QolInventoryItemRef item{};
    return contracts::succeeded(read_item(ref, &item)) && valid_handle(item.item);
}

InventoryActionStatus read_target_item(const QolActionRef& target, QolItemHandle* out) {
    QolInventoryItemRef item{};
    const InventoryActionStatus status = resolve_item(target, &item);
    if (status != InventoryActionStatus::kOk) return status;
    *out = item.item;
    return InventoryActionStatus::kOk;
}

contracts::InventoryMoveOutcome move(QolActionRef from, int32_t count, QolActionRef to) {
    contracts::InventoryMoveOutcome outcome{};
    outcome.status = InventoryActionStatus::kInvalidRef;
    outcome.target = to;
    if (count <= 0 || validate_action_ref(from) == InventoryActionStatus::kInvalidRef ||
        validate_action_ref(to) == InventoryActionStatus::kInvalidRef) {
        return outcome;
    }
    const InventoryActionStatus from_validation = validate_action_ref(from);
    const InventoryActionStatus to_validation = validate_action_ref(to);
    if (from_validation != InventoryActionStatus::kOk) {
        outcome.status = from_validation;
        return outcome;
    }
    if (to_validation != InventoryActionStatus::kOk) {
        outcome.status = to_validation;
        return outcome;
    }
    const InventoryActionStatus world_status = validate_world();
    if (world_status != InventoryActionStatus::kOk) {
        outcome.status = world_status;
        return outcome;
    }

    QolInventoryItemRef source_item{};
    const InventoryActionStatus source_status = resolve_item(from, &source_item);
    if (source_status != InventoryActionStatus::kOk) {
        outcome.status = source_status;
        return outcome;
    }
    if (from.space_id == to.space_id && from.bag == to.bag && from.slot == to.slot) {
        outcome.status = InventoryActionStatus::kConflict;
        return outcome;
    }
    const bool was_occupied = target_occupied(to);
    QolResult operation = contracts::ok();
    QolBagMutationResult target_result{};

    if (from.space_id == to.space_id && native_space(from.space_id)) {
        if (g_item_state_write == nullptr || g_item_state_write->move_item == nullptr) {
            outcome.status = InventoryActionStatus::kInternal;
            return outcome;
        }
        contracts::QolItemInventoryStateWriteResult write_result{};
        operation = g_item_state_write->move_item(
            native_bag_number(from), from.slot, count, native_bag_number(to), to.slot,
            &write_result);
    } else if (from.space_id == to.space_id) {
        const contracts::QolBagSource* source = find_bag_source(from.space_id);
        if (source == nullptr || source->export_item == nullptr || source->import_item == nullptr ||
            source->remove == nullptr) {
            outcome.status = InventoryActionStatus::kUnsupported;
            return outcome;
        }
        if (count > source_item.count) {
            outcome.status = InventoryActionStatus::kInvalidRef;
            return outcome;
        }
        contracts::QolBagTransfer transfer{};
        operation = source->export_item(source_item.item, count, &transfer);
        if (!contracts::succeeded(operation)) {
            outcome.status = status_from_result(operation, true);
            return outcome;
        }
        if (transfer.category != source_item.category || transfer.count != count ||
            transfer.payload_size == 0 ||
            transfer.payload_size > contracts::kQolBagTransferPayloadMaxSize) {
            outcome.status = InventoryActionStatus::kInternal;
            return outcome;
        }
        operation = source->import_item(&transfer, to.bag, to.slot, &target_result);
        if (!contracts::succeeded(operation)) {
            outcome.status = status_from_result(operation, true);
            return outcome;
        }
        if (!valid_handle(target_result.item) || target_result.ref.space_id != to.space_id ||
            target_result.ref.bag != to.bag) {
            outcome.status = InventoryActionStatus::kInternal;
            return outcome;
        }
        QolBagMutationResult removed{};
        operation = source->remove(source_item.item, count, &removed);
        if (!contracts::succeeded(operation)) {
            QolBagMutationResult compensation{};
            const QolResult rollback = source->remove(target_result.item, count, &compensation);
            outcome.status = contracts::succeeded(rollback)
                                 ? status_from_result(operation, true)
                                 : InventoryActionStatus::kInternal;
            return outcome;
        }
    } else {
        operation = usecases::move_item(
            {from.space_id, from.bag}, from.slot, count, {to.space_id, to.bag}, to.slot,
            &target_result);
        if (!contracts::succeeded(operation)) {
            outcome.status = status_from_result(operation, true);
            return outcome;
        }
    }

    if (!contracts::succeeded(operation)) {
        outcome.status = status_from_result(operation, true);
        return outcome;
    }
    outcome.status = InventoryActionStatus::kOk;
    outcome.moved_count = count;
    outcome.merged = was_occupied ? 1u : 0u;
    const InventoryActionStatus target_status = read_target_item(to, &outcome.target_item);
    if (target_status != InventoryActionStatus::kOk) outcome.status = target_status;
    request_inventory_refresh();
    return outcome;
}

contracts::InventoryUseOutcome use(QolActionRef ref) {
    contracts::InventoryUseOutcome outcome{InventoryActionStatus::kInvalidRef, 0};
    outcome.status = validate_action_ref(ref);
    if (outcome.status != InventoryActionStatus::kOk) return outcome;
    outcome.status = validate_world();
    if (outcome.status != InventoryActionStatus::kOk) return outcome;
    QolInventoryItemRef before{};
    outcome.status = resolve_item(ref, &before);
    if (outcome.status != InventoryActionStatus::kOk) return outcome;

    QolResult operation{};
    if (native_space(ref.space_id)) {
        if (g_item_state_write == nullptr || g_item_state_write->use_item == nullptr) {
            outcome.status = InventoryActionStatus::kInternal;
            return outcome;
        }
        contracts::QolItemInventoryStateWriteResult write_result{};
        operation = g_item_state_write->use_item(native_bag_number(ref), ref.slot, &write_result);
    } else {
        const contracts::QolBagSource* source = find_bag_source(ref.space_id);
        if (source == nullptr || source->use == nullptr) {
            outcome.status = InventoryActionStatus::kUnsupported;
            return outcome;
        }
        operation = source->use({ref.space_id, ref.bag}, ref.slot);
    }
    if (!contracts::succeeded(operation)) {
        outcome.status = status_from_result(operation, true);
        return outcome;
    }
    QolInventoryItemRef after{};
    const QolResult read_after = read_item(ref, &after);
    if (read_after.code == kQolErrNotFound) {
        outcome.consumed_count = before.count;
    } else if (contracts::succeeded(read_after) && valid_handle(after.item) &&
               after.item.slot == before.item.slot &&
               after.item.generation == before.item.generation) {
        outcome.consumed_count = before.count > after.count ? before.count - after.count : 0;
    } else {
        outcome.status = status_from_result(read_after, true);
        return outcome;
    }
    outcome.status = InventoryActionStatus::kOk;
    request_inventory_refresh();
    return outcome;
}

contracts::InventoryUseOutcome discard(QolActionRef ref, int32_t count) {
    contracts::InventoryUseOutcome outcome{InventoryActionStatus::kInvalidRef, 0};
    outcome.status = validate_action_ref(ref);
    if (outcome.status != InventoryActionStatus::kOk) return outcome;
    if (count <= 0) return outcome;
    outcome.status = validate_world();
    if (outcome.status != InventoryActionStatus::kOk) return outcome;
    QolInventoryItemRef item{};
    outcome.status = resolve_item(ref, &item);
    if (outcome.status != InventoryActionStatus::kOk) return outcome;
    if (count > item.count) return outcome;

    QolResult operation{};
    if (native_space(ref.space_id)) {
        if (g_item_state_write == nullptr || g_item_state_write->remove_item == nullptr) {
            outcome.status = InventoryActionStatus::kInternal;
            return outcome;
        }
        contracts::QolItemInventoryStateWriteResult write_result{};
        operation = g_item_state_write->remove_item(item.item, count, &write_result);
    } else {
        const contracts::QolBagSource* source = find_bag_source(ref.space_id);
        if (source == nullptr || source->remove == nullptr) {
            outcome.status = InventoryActionStatus::kUnsupported;
            return outcome;
        }
        QolBagMutationResult mutation{};
        operation = source->remove(item.item, count, &mutation);
    }
    if (!contracts::succeeded(operation)) {
        outcome.status = status_from_result(operation, true);
        return outcome;
    }
    outcome.status = InventoryActionStatus::kOk;
    outcome.consumed_count = count;
    request_inventory_refresh();
    return outcome;
}

contracts::InventorySellResult sell(QolActionRef ref, QolItemHandle expected_item,
                                    int32_t count, const char* source) {
    contracts::InventorySellResult outcome =
        inventory_hooks::inventory_action_sell(ref, expected_item, count, source);
    if (outcome.status == contracts::InventorySellStatus::kOk) request_inventory_refresh();
    return outcome;
}

contracts::InventoryUseOutcome equip(int32_t role, QolActionRef ref, int32_t equip_slot) {
    contracts::InventoryUseOutcome outcome{InventoryActionStatus::kInvalidRef, 0};
    outcome.status = validate_action_ref(ref);
    if (outcome.status != InventoryActionStatus::kOk) return outcome;
    if (role < 0 || equip_slot < 0) return outcome;
    outcome.status = validate_world();
    if (outcome.status != InventoryActionStatus::kOk) return outcome;
    if (!native_space(ref.space_id)) {
        // Host equipment operations currently accept only native bag handles.
        outcome.status = InventoryActionStatus::kUnsupported;
        return outcome;
    }
    QolInventoryItemRef item{};
    outcome.status = resolve_item(ref, &item);
    if (outcome.status != InventoryActionStatus::kOk) return outcome;
    if (g_item_state_write == nullptr || g_item_state_write->equip_item == nullptr) {
        outcome.status = InventoryActionStatus::kInternal;
        return outcome;
    }
    contracts::QolItemInventoryStateWriteResult write_result{};
    const QolResult operation = g_item_state_write->equip_item(
        role, native_bag_number(ref), ref.slot, nullptr, &write_result);
    outcome.status = contracts::succeeded(operation)
                         ? InventoryActionStatus::kOk
                         : status_from_result(operation, true);
    if (outcome.status == InventoryActionStatus::kOk) request_inventory_refresh();
    return outcome;
}

contracts::InventoryUseOutcome unequip(int32_t role, int32_t equip_slot) {
    contracts::InventoryUseOutcome outcome{InventoryActionStatus::kInvalidRef, 0};
    if (role < 0 || equip_slot < 0) return outcome;
    outcome.status = validate_world();
    if (outcome.status != InventoryActionStatus::kOk) return outcome;
    // Host equipment operations currently accept only native bag handles.
    if (g_item_state_write == nullptr || g_item_state_write->unequip_item == nullptr) {
        outcome.status = InventoryActionStatus::kInternal;
        return outcome;
    }
    contracts::QolItemInventoryStateWriteResult write_result{};
    const QolResult operation =
        g_item_state_write->unequip_item(role, equip_slot, nullptr, &write_result);
    outcome.status = contracts::succeeded(operation)
                         ? InventoryActionStatus::kOk
                         : status_from_result(operation, true);
    if (outcome.status == InventoryActionStatus::kOk) request_inventory_refresh();
    return outcome;
}

contracts::InventoryUseOutcome put_jewel(int32_t role, QolActionRef jewel,
                                         int32_t equip_slot) {
    contracts::InventoryUseOutcome outcome{InventoryActionStatus::kInvalidRef, 0};
    outcome.status = validate_action_ref(jewel);
    if (outcome.status != InventoryActionStatus::kOk) return outcome;
    if (role < 0 || equip_slot < 0) return outcome;
    outcome.status = validate_world();
    if (outcome.status != InventoryActionStatus::kOk) return outcome;
    if (!native_space(jewel.space_id)) {
        // Host equipment operations currently accept only native bag handles.
        outcome.status = InventoryActionStatus::kUnsupported;
        return outcome;
    }
    QolInventoryItemRef item{};
    outcome.status = resolve_item(jewel, &item);
    if (outcome.status != InventoryActionStatus::kOk) return outcome;
    if (g_item_state_write == nullptr || g_item_state_write->put_jewel == nullptr) {
        outcome.status = InventoryActionStatus::kInternal;
        return outcome;
    }
    contracts::QolItemInventoryStateWriteResult write_result{};
    const QolResult operation = g_item_state_write->put_jewel(
        role, native_bag_number(jewel), jewel.slot, equip_slot, nullptr, &write_result);
    outcome.status = contracts::succeeded(operation)
                         ? InventoryActionStatus::kOk
                         : status_from_result(operation, true);
    if (outcome.status == InventoryActionStatus::kOk) request_inventory_refresh();
    return outcome;
}

}  // namespace

const contracts::InventoryActionApi kInventoryActionApi{
    sizeof(contracts::InventoryActionApi),
    contracts::kInventoryActionVersionMajor,
    contracts::kInventoryActionVersionMinor,
    &move,
    &use,
    &discard,
    &sell,
    &equip,
    &unequip,
    &put_jewel,
};

}  // namespace qol::features::core_item
