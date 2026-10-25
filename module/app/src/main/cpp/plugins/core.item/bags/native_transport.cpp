#include "native_transport.h"

#include "plugins/core.item/runtime/core_item_internal.h"

namespace qol::features::core_item::bags::native_transport {
namespace {

QolResult unsupported(const char* message, uint32_t size) {
    return contracts::error(kQolErrUnsupported, message, size);
}

QolResult internal_error(const char* message, uint32_t size) {
    return contracts::error(kQolErrInternal, message, size);
}

bool is_native_space(int32_t space) {
    return space == contracts::kQolBagSpaceOriginal ||
           space == contracts::kQolBagSpaceC18;
}

bool valid_handle(contracts::QolItemHandle item) {
    return item.slot != 0 && item.generation != 0;
}

QolResult invalid_argument() {
    constexpr char kMessage[] = "invalid native item transfer";
    return contracts::error(kQolErrInvalidArgument, kMessage,
                            static_cast<uint32_t>(sizeof(kMessage) - 1));
}

QolResult find_empty_bag(int32_t space, contracts::QolBagRef* out_ref) {
    if (out_ref == nullptr || g_port == nullptr ||
        !contracts::qol_item_port_bag_space_size_valid(g_port) ||
        g_port->read.bag_count == nullptr || g_port->read.bag_info == nullptr ||
        g_port->read.item_at == nullptr) {
        constexpr char kMessage[] = "native bag scan unavailable";
        return unsupported(kMessage, static_cast<uint32_t>(sizeof(kMessage) - 1));
    }

    int32_t bag_count = 0;
    QolResult result = g_port->read.bag_count(space, &bag_count);
    if (!contracts::succeeded(result)) return result;
    if (bag_count < 0) {
        constexpr char kMessage[] = "invalid native bag count";
        return internal_error(kMessage, static_cast<uint32_t>(sizeof(kMessage) - 1));
    }

    for (int32_t bag = 0; bag < bag_count; ++bag) {
        if (space == contracts::kQolBagSpaceOriginal &&
            bag == contracts::kQolTaskBagIndex) {
            continue;
        }
        const contracts::QolBagRef ref{space, bag};
        contracts::QolBagInfo info{};
        result = g_port->read.bag_info(&ref, &info);
        if (!contracts::succeeded(result)) return result;
        if (info.capacity < 0) {
            constexpr char kMessage[] = "invalid native bag capacity";
            return internal_error(kMessage, static_cast<uint32_t>(sizeof(kMessage) - 1));
        }
        for (int32_t slot = 0; slot < info.capacity; ++slot) {
            contracts::QolInventoryItemRef item{};
            result = g_port->read.item_at(&ref, slot, &item);
            if (result.code == kQolErrNotFound) {
                *out_ref = ref;
                return contracts::ok();
            }
            if (!contracts::succeeded(result)) return result;
        }
    }
    constexpr char kMessage[] = "native inventory full";
    return contracts::error(kQolErrBlocked, kMessage,
                            static_cast<uint32_t>(sizeof(kMessage) - 1));
}

}  // namespace

QolResult export_item(int32_t space, contracts::QolItemHandle item, int32_t count,
                      BagTransfer* out) {
    if (out == nullptr || !is_native_space(space) || !valid_handle(item) || count <= 0) {
        return invalid_argument();
    }
    if (g_port == nullptr || !contracts::qol_item_port_transfer_size_valid(g_port) ||
        g_port->read.serialize_item == nullptr) {
        constexpr char kMessage[] = "item serialization unavailable";
        return unsupported(kMessage, static_cast<uint32_t>(sizeof(kMessage) - 1));
    }
    if (g_port->read.category == nullptr) {
        constexpr char kMessage[] = "item category unavailable";
        return unsupported(kMessage, static_cast<uint32_t>(sizeof(kMessage) - 1));
    }

    uint32_t required_size = 0;
    QolResult result = g_port->read.serialize_item(item, nullptr, 0, &required_size);
    if (!contracts::succeeded(result)) return result;
    if (required_size == 0 || required_size > contracts::kQolBagTransferPayloadMaxSize) {
        constexpr char kMessage[] = "invalid serialized item size";
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
        constexpr char kMessage[] = "invalid serialized item size";
        return internal_error(kMessage, static_cast<uint32_t>(sizeof(kMessage) - 1));
    }

    int32_t category = 0;
    result = g_port->read.category(item, &category);
    if (!contracts::succeeded(result)) return result;
    if (category <= 0) {
        constexpr char kMessage[] = "invalid item category";
        return internal_error(kMessage, static_cast<uint32_t>(sizeof(kMessage) - 1));
    }
    transfer.category = category;
    transfer.count = count;
    transfer.payload_size = serialized_size;
    *out = transfer;
    return contracts::ok();
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
        constexpr char kMessage[] = "item loading unavailable";
        return unsupported(kMessage, static_cast<uint32_t>(sizeof(kMessage) - 1));
    }
    if (g_item_state_write->move_handle == nullptr) {
        constexpr char kMessage[] = "item placement unavailable";
        return unsupported(kMessage, static_cast<uint32_t>(sizeof(kMessage) - 1));
    }

    contracts::QolBagRef ref{target_space, preferred_bag};
    if (preferred_bag == -1) {
        const QolResult selected = find_empty_bag(target_space, &ref);
        if (!contracts::succeeded(selected)) return selected;
    }

    contracts::QolItemHandle item{};
    QolResult result = g_item_state_write->load_item(transfer.payload,
                                                      transfer.payload_size, &item);
    if (!contracts::succeeded(result)) return result;

    contracts::QolItemInventoryStateWriteResult write_result{};
    // move_handle routes through the game's move primitive: an empty target receives the
    // stack, an occupied same-category target merges, and a full/conflicting target fails.
    result = g_item_state_write->move_handle(item, transfer.count, &ref, preferred_slot,
                                             &write_result);
    if (!contracts::succeeded(result)) {
        // move_handle keeps ownership on failure; drop the detached object to avoid a leak.
        if (g_item_state_write->remove_item != nullptr) {
            contracts::QolItemInventoryStateWriteResult discarded{};
            (void)g_item_state_write->remove_item(item, transfer.count, &discarded);
        }
        // Host fills result.message from write_result's local storage; never return it.
        constexpr char kMessage[] = "target bag rejected the transferred item";
        return contracts::error(result.code, kMessage,
                                static_cast<uint32_t>(sizeof(kMessage) - 1));
    }

    int32_t actual_slot = preferred_slot;
    if (actual_slot < 0) {
        contracts::QolBagRef actual_ref{};
        const QolResult located = locate(target_space, item, &actual_ref, &actual_slot);
        if (!contracts::succeeded(located)) return located;
        ref = actual_ref;
    }
    *out = contracts::QolBagMutationResult{ref, item, transfer.count};
    return contracts::ok();
}

QolResult remove_item(int32_t space, contracts::QolItemHandle expected, int32_t count,
                      contracts::QolBagMutationResult* out) {
    if (out == nullptr || !is_native_space(space) || !valid_handle(expected) || count <= 0) {
        return invalid_argument();
    }
    if (g_item_state_write == nullptr || g_item_state_write->remove_item == nullptr) {
        constexpr char kMessage[] = "item removal unavailable";
        return unsupported(kMessage, static_cast<uint32_t>(sizeof(kMessage) - 1));
    }

    contracts::QolBagRef ref{};
    int32_t slot = -1;
    QolResult result = locate(space, expected, &ref, &slot);
    if (!contracts::succeeded(result)) return result;

    contracts::QolItemInventoryStateWriteResult write_result{};
    result = g_item_state_write->remove_item(expected, count, &write_result);
    if (!contracts::succeeded(result)) {
        constexpr char kMessage[] = "source bag rejected the item removal";
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
    if (g_port == nullptr || !contracts::qol_item_port_bag_space_size_valid(g_port) ||
        g_port->read.bag_count == nullptr || g_port->read.bag_info == nullptr ||
        g_port->read.item_at == nullptr) {
        constexpr char kMessage[] = "native bag scan unavailable";
        return unsupported(kMessage, static_cast<uint32_t>(sizeof(kMessage) - 1));
    }

    int32_t bag_count = 0;
    QolResult result = g_port->read.bag_count(space, &bag_count);
    if (!contracts::succeeded(result)) return result;
    if (bag_count < 0) {
        constexpr char kMessage[] = "invalid native bag count";
        return internal_error(kMessage, static_cast<uint32_t>(sizeof(kMessage) - 1));
    }

    for (int32_t bag = 0; bag < bag_count; ++bag) {
        const contracts::QolBagRef ref{space, bag};
        contracts::QolBagInfo info{};
        result = g_port->read.bag_info(&ref, &info);
        if (!contracts::succeeded(result)) return result;
        if (info.capacity < 0) {
            constexpr char kMessage[] = "invalid native bag capacity";
            return internal_error(kMessage, static_cast<uint32_t>(sizeof(kMessage) - 1));
        }
        for (int32_t slot = 0; slot < info.capacity; ++slot) {
            contracts::QolInventoryItemRef candidate{};
            result = g_port->read.item_at(&ref, slot, &candidate);
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
    constexpr char kMessage[] = "native item not found";
    return contracts::error(kQolErrNotFound, kMessage,
                            static_cast<uint32_t>(sizeof(kMessage) - 1));
}

}  // namespace qol::features::core_item::bags::native_transport
