#include "native_transport.h"
#include "plugins/core.inventory/api/access.h"

#include "plugins/core.inventory/hooks/placement.h"
#include "plugins/core.inventory/runtime/context.h"

namespace qol::features::core_inventory::transfer::native_transport {
namespace {

namespace placement = qol::features::core_inventory::hooks::placement;

QolResult unsupported(const char* message, uint32_t size) {
    return contracts::error(kQolErrUnsupported, message, size);
}

QolResult internal_error(const char* message, uint32_t size) {
    return contracts::error(kQolErrInternal, message, size);
}

bool is_native_space(int32_t space) {
    return space == contracts::kQolBagSpaceOriginal ||
           space == contracts::kQolBagSpaceVariant;
}

bool valid_handle(contracts::QolItemHandle item) {
    return item.slot != 0 && item.generation != 0;
}

QolResult invalid_argument() {
    static constexpr char kMessage[] = "invalid native item transfer";
    return contracts::error(kQolErrInvalidArgument, kMessage,
                            static_cast<uint32_t>(sizeof(kMessage) - 1));
}

QolResult find_empty_bag(int32_t space, contracts::QolBagRef* out_ref) {
    if (out_ref == nullptr ||
        !contracts::qol_inventory_access_api_valid(&inventory_access_api())) {
        static constexpr char kMessage[] = "native bag scan unavailable";
        return unsupported(kMessage, static_cast<uint32_t>(sizeof(kMessage) - 1));
    }

    int32_t bag_count = 0;
    QolResult result = inventory_access_api().native_bag_count(space, &bag_count);
    if (!contracts::succeeded(result)) return result;
    if (bag_count < 0) {
        static constexpr char kMessage[] = "invalid native bag count";
        return internal_error(kMessage, static_cast<uint32_t>(sizeof(kMessage) - 1));
    }

    for (int32_t bag = 0; bag < bag_count; ++bag) {
        if (space == contracts::kQolBagSpaceOriginal &&
            bag == contracts::kQolTaskBagIndex) {
            continue;
        }
        const contracts::QolBagRef ref{space, bag};
        contracts::QolBagInfo info{};
        result = inventory_access_api().native_bag_info(&ref, &info);
        if (!contracts::succeeded(result)) return result;
        if (info.capacity < 0) {
            static constexpr char kMessage[] = "invalid native bag capacity";
            return internal_error(kMessage, static_cast<uint32_t>(sizeof(kMessage) - 1));
        }
        for (int32_t slot = 0; slot < info.capacity; ++slot) {
            contracts::QolInventoryItemRef item{};
            result = inventory_access_api().native_item_at(&ref, slot, &item);
            if (result.code == kQolErrNotFound) {
                *out_ref = ref;
                return contracts::ok();
            }
            if (!contracts::succeeded(result)) return result;
        }
    }
    static constexpr char kMessage[] = "native inventory full";
    return contracts::error(kQolErrBlocked, kMessage,
                            static_cast<uint32_t>(sizeof(kMessage) - 1));
}

// 序列化整堆物品；来源是否为原生空间由调用方判定，句柄已携带物品身份。
QolResult serialize_handle(contracts::QolItemHandle item, int32_t count, BagTransfer* out) {
    if (out == nullptr || !valid_handle(item) || count <= 0) {
        return invalid_argument();
    }
    if (g_port == nullptr || !contracts::qol_item_port_transfer_size_valid(g_port) ||
        g_port->read.serialize_item == nullptr) {
        static constexpr char kMessage[] = "item serialization unavailable";
        return unsupported(kMessage, static_cast<uint32_t>(sizeof(kMessage) - 1));
    }
    if (g_port->read.category == nullptr) {
        static constexpr char kMessage[] = "item category unavailable";
        return unsupported(kMessage, static_cast<uint32_t>(sizeof(kMessage) - 1));
    }

    uint32_t required_size = 0;
    QolResult result = g_port->read.serialize_item(item, nullptr, 0, &required_size);
    if (!contracts::succeeded(result)) return result;
    if (required_size == 0 || required_size > contracts::kQolBagTransferPayloadMaxSize) {
        static constexpr char kMessage[] = "invalid serialized item size";
        return internal_error(kMessage, static_cast<uint32_t>(sizeof(kMessage) - 1));
    }

    BagTransfer transfer{};
    uint32_t serialized_size = 0;
    result = g_port->read.serialize_item(item, transfer.payload,
                                         contracts::kQolBagTransferPayloadCapacity,
                                         &serialized_size);
    if (!contracts::succeeded(result)) return result;
    if (serialized_size != required_size || serialized_size == 0 ||
        serialized_size > contracts::kQolBagTransferPayloadMaxSize) {
        static constexpr char kMessage[] = "invalid serialized item size";
        return internal_error(kMessage, static_cast<uint32_t>(sizeof(kMessage) - 1));
    }

    int32_t category = 0;
    result = g_port->read.category(item, &category);
    if (!contracts::succeeded(result)) return result;
    if (category <= 0) {
        static constexpr char kMessage[] = "invalid item category";
        return internal_error(kMessage, static_cast<uint32_t>(sizeof(kMessage) - 1));
    }
    transfer.category = category;
    transfer.count = count;
    transfer.payload_size = serialized_size;
    *out = transfer;
    return contracts::ok();
}

}  // namespace

QolResult export_item(int32_t space, contracts::QolItemHandle item, int32_t count,
                      BagTransfer* out) {
    if (!is_native_space(space)) return invalid_argument();
    return serialize_handle(item, count, out);
}

QolResult export_detached_item(contracts::QolItemHandle item, int32_t count, BagTransfer* out) {
    return serialize_handle(item, count, out);
}

QolResult import_item(const BagTransfer& transfer, int32_t target_space,
                      int32_t preferred_bag, int32_t preferred_slot,
                      contracts::QolBagMutationResult* out) {
    if (out == nullptr || !is_native_space(target_space) || transfer.category <= 0 ||
        transfer.count <= 0 || transfer.payload_size == 0 ||
        transfer.payload_size > contracts::kQolBagTransferPayloadMaxSize ||
        preferred_bag < -1 || preferred_slot < -1 ||
        (preferred_bag == -1 && preferred_slot != -1) || preferred_slot >= 16) {
        return invalid_argument();
    }
    if (g_item_state_write == nullptr || g_item_state_write->load_item == nullptr) {
        static constexpr char kMessage[] = "item loading unavailable";
        return unsupported(kMessage, static_cast<uint32_t>(sizeof(kMessage) - 1));
    }
    if (g_item_state_write->move_handle == nullptr) {
        static constexpr char kMessage[] = "item placement unavailable";
        return unsupported(kMessage, static_cast<uint32_t>(sizeof(kMessage) - 1));
    }

    contracts::QolBagRef ref{target_space, preferred_bag};
    if (preferred_bag == -1) {
        // detached 堆不在任何袋内，无自并风险；自动落袋统一走 placement resolver
        // 的 scope 版本（merge → empty），与 FindSaveSlot/move_item 同序。
        placement::PlacementTarget target{};
        if (placement::resolve_placement(transfer, transfer.count > 1, target_space, -1,
                                         &target)) {
            ref = contracts::QolBagRef{target.space_id, target.bag};
            preferred_slot = target.slot;
        } else {
            const QolResult selected = find_empty_bag(target_space, &ref);
            if (!contracts::succeeded(selected)) return selected;
        }
    }

    contracts::QolItemHandle item{};
    QolResult result = g_item_state_write->load_item(transfer.payload,
                                                      transfer.payload_size, &item);
    if (!contracts::succeeded(result)) return result;

    contracts::QolItemInventoryStateWriteResult write_result{};
    // move_handle places detached stacks into empty native slots and merges compatible stacks.
    result = g_item_state_write->move_handle(item, transfer.count, &ref, preferred_slot,
                                              &write_result);
    if (!contracts::succeeded(result)) {
        // move_handle's Host adapter releases the detached object on failure.
        static constexpr char kMessage[] = "target bag rejected the transferred item";
        return contracts::error(result.code, kMessage,
                                static_cast<uint32_t>(sizeof(kMessage) - 1));
    }

    int32_t actual_slot = preferred_slot;
    if (write_result.gained_count != 0) actual_slot = write_result.gained[0].slot;
    if (actual_slot < 0) {
        contracts::QolBagRef actual_ref{};
        const QolResult located = locate(target_space, item, &actual_ref, &actual_slot);
        if (!contracts::succeeded(located)) return located;
        ref = actual_ref;
    }

    contracts::QolItemHandle target_item = item;
    if (contracts::qol_inventory_access_api_valid(&inventory_access_api())) {
        contracts::QolInventoryItemRef target{};
        const QolResult read_target = inventory_access_api().native_item_at(&ref, actual_slot, &target);
        if (contracts::succeeded(read_target) && valid_handle(target.item)) {
            target_item = target.item;
        }
    }
    *out = contracts::QolBagMutationResult{ref, target_item, transfer.count};
    return contracts::ok();
}

QolResult remove_item(int32_t space, contracts::QolItemHandle expected, int32_t count,
                      contracts::QolBagMutationResult* out) {
    if (out == nullptr || !is_native_space(space) || !valid_handle(expected) || count <= 0) {
        return invalid_argument();
    }
    if (g_item_state_write == nullptr || g_item_state_write->remove_item == nullptr) {
        static constexpr char kMessage[] = "item removal unavailable";
        return unsupported(kMessage, static_cast<uint32_t>(sizeof(kMessage) - 1));
    }

    contracts::QolBagRef ref{};
    int32_t slot = -1;
    QolResult result = locate(space, expected, &ref, &slot);
    if (!contracts::succeeded(result)) return result;

    contracts::QolItemInventoryStateWriteResult write_result{};
    result = g_item_state_write->remove_item(expected, count, &write_result);
    if (!contracts::succeeded(result)) {
        static constexpr char kMessage[] = "source bag rejected the item removal";
        return contracts::error(result.code, kMessage,
                                static_cast<uint32_t>(sizeof(kMessage) - 1));
    }
    *out = contracts::QolBagMutationResult{ref, expected, count};
    return contracts::ok();
}

QolResult locate(int32_t space, contracts::QolItemHandle item,
                 contracts::QolBagRef* out, int32_t* out_slot) {
    if (out == nullptr || out_slot == nullptr || !is_native_space(space) ||
        !valid_handle(item)) {
        return invalid_argument();
    }
    if (!contracts::qol_inventory_access_api_valid(&inventory_access_api())) {
        static constexpr char kMessage[] = "native bag scan unavailable";
        return unsupported(kMessage, static_cast<uint32_t>(sizeof(kMessage) - 1));
    }

    int32_t bag_count = 0;
    QolResult result = inventory_access_api().native_bag_count(space, &bag_count);
    if (!contracts::succeeded(result)) return result;
    if (bag_count < 0) {
        static constexpr char kMessage[] = "invalid native bag count";
        return internal_error(kMessage, static_cast<uint32_t>(sizeof(kMessage) - 1));
    }

    for (int32_t bag = 0; bag < bag_count; ++bag) {
        const contracts::QolBagRef ref{space, bag};
        contracts::QolBagInfo info{};
        result = inventory_access_api().native_bag_info(&ref, &info);
        if (!contracts::succeeded(result)) return result;
        if (info.capacity < 0) {
            static constexpr char kMessage[] = "invalid native bag capacity";
            return internal_error(kMessage, static_cast<uint32_t>(sizeof(kMessage) - 1));
        }
        for (int32_t slot = 0; slot < info.capacity; ++slot) {
            contracts::QolInventoryItemRef candidate{};
            result = inventory_access_api().native_item_at(&ref, slot, &candidate);
            if (result.code == kQolErrNotFound) continue;
            if (!contracts::succeeded(result)) return result;
            if (candidate.item.slot == item.slot &&
                candidate.item.generation == item.generation) {
                *out = ref;
                *out_slot = slot;
                return contracts::ok();
            }
        }
    }
    static constexpr char kMessage[] = "native item not found";
    return contracts::error(kQolErrNotFound, kMessage,
                            static_cast<uint32_t>(sizeof(kMessage) - 1));
}

}  // namespace qol::features::core_inventory::transfer::native_transport
