#include "move_item.h"

#include "plugins/core.item/bags/source_transport.h"
#include "plugins/core.item/runtime/core_item_internal.h"
#include "plugins/core.item/usecases/transfer_journal.h"

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

}  // namespace

QolResult move_item(const contracts::QolBagRef& from, int32_t from_slot, int32_t count,
                    const contracts::QolBagRef& to, int32_t to_slot,
                    contracts::QolBagMutationResult* out_target) {
    if (out_target == nullptr || from.space_id < 0 || from.bag < 0 || from_slot < 0 ||
        to.space_id < 0 || to.bag < 0 || to_slot < 0 || count <= 0) {
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
    if (is_native_space(from.space_id) || is_native_space(to.space_id)) {
        constexpr char kMessage[] = "host item transfer primitive unavailable";
        return unsupported(kMessage, static_cast<uint32_t>(sizeof(kMessage) - 1));
    }
    if (from.space_id < contracts::kQolBagSpaceFirstRegistered ||
        to.space_id < contracts::kQolBagSpaceFirstRegistered) {
        return invalid_argument();
    }
    if (from.space_id == to.space_id && from.bag == to.bag && from_slot == to_slot) {
        constexpr char kMessage[] = "source and target are identical";
        return contracts::error(kQolErrConflict, kMessage,
                                static_cast<uint32_t>(sizeof(kMessage) - 1));
    }

    const contracts::QolBagSource* source = find_bag_source(from.space_id);
    if (source == nullptr || source->item_at == nullptr) {
        constexpr char kMessage[] = "source bag unavailable";
        return unsupported(kMessage, static_cast<uint32_t>(sizeof(kMessage) - 1));
    }
    contracts::QolInventoryItemRef source_item{};
    const QolResult source_read = source->item_at(from.bag, from_slot, &source_item);
    if (!contracts::succeeded(source_read)) return source_read;
    if (!valid_handle(source_item.item) || source_item.space_id != from.space_id ||
        source_item.bag != from.bag || source_item.slot != from_slot ||
        source_item.category <= 0) {
        constexpr char kMessage[] = "source bag returned invalid item";
        return contracts::error(kQolErrInternal, kMessage,
                                static_cast<uint32_t>(sizeof(kMessage) - 1));
    }

    transfer_journal::JournalId journal_id{};
    if (!transfer_journal::begin(from, from_slot, to, to_slot, source_item.category, count,
                                 &journal_id)) {
        constexpr char kMessage[] = "transfer journal unavailable";
        return contracts::error(kQolErrBlocked, kMessage,
                                static_cast<uint32_t>(sizeof(kMessage) - 1));
    }

    bags::BagTransfer transfer{};
    const QolResult exported = bags::source_transport::export_item(
        from.space_id, source_item.item, count, &transfer);
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
    const QolResult imported = bags::source_transport::import_item(
        transfer, to.space_id, to.bag, to_slot, &target_result);
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
    const QolResult removed = bags::source_transport::remove_item(
        from.space_id, source_item.item, count, &source_result);
    if (!contracts::succeeded(removed)) {
        contracts::QolBagMutationResult compensation{};
        const QolResult compensated = bags::source_transport::remove_item(
            to.space_id, target_result.item, count, &compensation);
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
