#include "move_item.h"

#include "plugins/core.item/bags/native_transport.h"
#include "plugins/core.item/bags/source_transport.h"
#include "plugins/core.item/runtime/core_item_internal.h"
#include "plugins/core.item/usecases/transfer_journal.h"
#if !defined(QOL_CORE_ITEM_TRANSFER_TRANSPORT_SEPARATE)
#include "plugins/core.item/bags/native_transport.cpp"
#endif

namespace qol::features::core_item::usecases {
namespace {

QolResult invalid_argument() {
    constexpr char kMessage[] = "invalid item transfer request";
    return contracts::error(kQolErrInvalidArgument, kMessage,
                            static_cast<uint32_t>(sizeof(kMessage) - 1));
}

QolResult unsupported(const char* message, uint32_t size) {
    return contracts::error(kQolErrUnsupported, message, size);
}

QolResult internal_error() {
    constexpr char kMessage[] = "item transfer rollback failed";
    return contracts::error(kQolErrInternal, kMessage,
                            static_cast<uint32_t>(sizeof(kMessage) - 1));
}

bool is_native_space(int32_t space) {
    return space == contracts::kQolBagSpaceOriginal ||
           space == contracts::kQolBagSpaceC18;
}

bool valid_handle(contracts::QolItemHandle item) {
    return item.slot != 0 && item.generation != 0;
}

QolResult source_item_at(const contracts::QolBagRef& ref, int32_t slot,
                         contracts::QolInventoryItemRef* out) {
    if (is_native_space(ref.space_id)) {
        if (g_port == nullptr || !contracts::qol_item_port_bag_space_size_valid(g_port) ||
            g_port->read.item_at == nullptr) {
            constexpr char kMessage[] = "native bag read unavailable";
            return unsupported(kMessage, static_cast<uint32_t>(sizeof(kMessage) - 1));
        }
        return g_port->read.item_at(&ref, slot, out);
    }
    const contracts::QolBagSource* source = find_bag_source(ref.space_id);
    if (source == nullptr || source->item_at == nullptr) {
        constexpr char kMessage[] = "source bag unavailable";
        return unsupported(kMessage, static_cast<uint32_t>(sizeof(kMessage) - 1));
    }
    return source->item_at(ref.bag, slot, out);
}

QolResult find_empty_slot(const contracts::QolBagRef& ref, int32_t* out_slot) {
    if (out_slot == nullptr) return invalid_argument();
    contracts::QolBagInfo info{};
    QolResult result{};
    if (is_native_space(ref.space_id)) {
        if (g_port == nullptr || !contracts::qol_item_port_bag_space_size_valid(g_port) ||
            g_port->read.bag_info == nullptr || g_port->read.item_at == nullptr) {
            constexpr char kMessage[] = "native bag read unavailable";
            return unsupported(kMessage, static_cast<uint32_t>(sizeof(kMessage) - 1));
        }
        result = g_port->read.bag_info(&ref, &info);
    } else {
        const contracts::QolBagSource* source = find_bag_source(ref.space_id);
        if (source == nullptr || source->info == nullptr || source->item_at == nullptr) {
            constexpr char kMessage[] = "target bag unavailable";
            return unsupported(kMessage, static_cast<uint32_t>(sizeof(kMessage) - 1));
        }
        result = source->info(ref.bag, &info);
        if (!contracts::succeeded(result)) return result;
        if (info.capacity < 0) {
            constexpr char kMessage[] = "invalid target bag capacity";
            return contracts::error(kQolErrInternal, kMessage,
                                    static_cast<uint32_t>(sizeof(kMessage) - 1));
        }
        for (int32_t slot = 0; slot < info.capacity; ++slot) {
            contracts::QolInventoryItemRef item{};
            result = source->item_at(ref.bag, slot, &item);
            if (result.code == kQolErrNotFound) {
                *out_slot = slot;
                return contracts::ok();
            }
            if (!contracts::succeeded(result)) return result;
        }
        constexpr char kMessage[] = "target bag full";
        return contracts::error(kQolErrBlocked, kMessage,
                                static_cast<uint32_t>(sizeof(kMessage) - 1));
    }
    if (!contracts::succeeded(result)) return result;
    if (info.capacity < 0) {
        constexpr char kMessage[] = "invalid target bag capacity";
        return contracts::error(kQolErrInternal, kMessage,
                                static_cast<uint32_t>(sizeof(kMessage) - 1));
    }
    for (int32_t slot = 0; slot < info.capacity; ++slot) {
        contracts::QolInventoryItemRef item{};
        result = g_port->read.item_at(&ref, slot, &item);
        if (result.code == kQolErrNotFound) {
            *out_slot = slot;
            return contracts::ok();
        }
        if (!contracts::succeeded(result)) return result;
    }
    constexpr char kMessage[] = "target bag full";
    return contracts::error(kQolErrBlocked, kMessage,
                            static_cast<uint32_t>(sizeof(kMessage) - 1));
}

}  // namespace

QolResult move_item(const contracts::QolBagRef& from, int32_t from_slot, int32_t count,
                    const contracts::QolBagRef& to, int32_t to_slot,
                    contracts::QolBagMutationResult* out_target) {
    if (out_target == nullptr || from.space_id < 0 || from.bag < 0 || from_slot < 0 ||
        to.space_id < 0 || to.bag < 0 || to_slot < -1 || count <= 0) {
        return invalid_argument();
    }
    if (contracts::qol_bag_ref_is_task_bag(from) || contracts::qol_bag_ref_is_task_bag(to)) {
        constexpr char kMessage[] = "task bag excluded";
        return contracts::error(kQolErrBlocked, kMessage,
                                static_cast<uint32_t>(sizeof(kMessage) - 1));
    }
    if (from.space_id == to.space_id && is_native_space(from.space_id)) {
        constexpr char kMessage[] = "native move path owned by host";
        return unsupported(kMessage, static_cast<uint32_t>(sizeof(kMessage) - 1));
    }
    if ((!is_native_space(from.space_id) &&
         from.space_id < contracts::kQolBagSpaceFirstRegistered) ||
        (!is_native_space(to.space_id) &&
         to.space_id < contracts::kQolBagSpaceFirstRegistered)) {
        return invalid_argument();
    }
    if (from.space_id == to.space_id && from.bag == to.bag && from_slot == to_slot) {
        constexpr char kMessage[] = "source and target are identical";
        return contracts::error(kQolErrConflict, kMessage,
                                static_cast<uint32_t>(sizeof(kMessage) - 1));
    }

    int32_t target_slot = to_slot;
    if (target_slot == -1) {
        const QolResult selected = find_empty_slot(to, &target_slot);
        if (!contracts::succeeded(selected)) return selected;
    }

    contracts::QolInventoryItemRef source_item{};
    const QolResult source_read = source_item_at(from, from_slot, &source_item);
    if (!contracts::succeeded(source_read)) return source_read;
    if (!valid_handle(source_item.item) || source_item.space_id != from.space_id ||
        source_item.bag != from.bag || source_item.slot != from_slot ||
        source_item.category <= 0) {
        constexpr char kMessage[] = "source bag returned invalid item";
        return contracts::error(kQolErrInternal, kMessage,
                                static_cast<uint32_t>(sizeof(kMessage) - 1));
    }

    transfer_journal::JournalId journal_id{};
    if (!transfer_journal::begin(from, from_slot, to, target_slot, source_item.category, count,
                                 &journal_id)) {
        constexpr char kMessage[] = "transfer journal unavailable";
        return contracts::error(kQolErrBlocked, kMessage,
                                static_cast<uint32_t>(sizeof(kMessage) - 1));
    }

    bags::BagTransfer transfer{};
    const auto export_source = is_native_space(from.space_id)
                                   ? &bags::native_transport::export_item
                                   : &bags::source_transport::export_item;
    const QolResult exported = export_source(from.space_id, source_item.item, count, &transfer);
    if (!contracts::succeeded(exported)) {
        transfer_journal::clear(journal_id);
        return exported;
    }
    if (transfer.category != source_item.category || transfer.count != count ||
        transfer.payload_size == 0 ||
        transfer.payload_size > contracts::kQolBagTransferPayloadMaxSize) {
        transfer_journal::clear(journal_id);
        constexpr char kMessage[] = "source returned invalid transfer payload";
        return contracts::error(kQolErrInternal, kMessage,
                                static_cast<uint32_t>(sizeof(kMessage) - 1));
    }

    contracts::QolBagMutationResult target_result{};
    const auto import_target = is_native_space(to.space_id)
                                   ? &bags::native_transport::import_item
                                   : &bags::source_transport::import_item;
    const QolResult imported =
        import_target(transfer, to.space_id, to.bag, target_slot, &target_result);
    if (!contracts::succeeded(imported)) {
        transfer_journal::clear(journal_id);
        return imported;
    }
    if (!valid_handle(target_result.item) || target_result.ref.space_id != to.space_id ||
        target_result.ref.bag != to.bag || target_result.count != count) {
        transfer_journal::mark_target_written(journal_id, bags::payload_hash(transfer));
        return internal_error();
    }
    transfer_journal::mark_target_written(journal_id, bags::payload_hash(transfer));

    contracts::QolBagMutationResult source_result{};
    const auto remove_source = is_native_space(from.space_id)
                                   ? &bags::native_transport::remove_item
                                   : &bags::source_transport::remove_item;
    const QolResult removed = remove_source(from.space_id, source_item.item, count,
                                             &source_result);
    if (!contracts::succeeded(removed)) {
        contracts::QolBagMutationResult compensation{};
        const auto remove_target = is_native_space(to.space_id)
                                       ? &bags::native_transport::remove_item
                                       : &bags::source_transport::remove_item;
        const QolResult compensated = remove_target(to.space_id, target_result.item, count,
                                                     &compensation);
        if (contracts::succeeded(compensated)) {
            transfer_journal::clear(journal_id);
            return removed;
        }
        return internal_error();
    }

    transfer_journal::clear(journal_id);
    *out_target = target_result;
    request_inventory_refresh();
    return contracts::ok();
}

}  // namespace qol::features::core_item::usecases
