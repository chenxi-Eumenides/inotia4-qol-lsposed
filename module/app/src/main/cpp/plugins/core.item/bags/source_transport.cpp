#include "source_transport.h"

#include "plugins/core.item/runtime/core_item_internal.h"

namespace qol::features::core_item::bags::source_transport {
namespace {

QolResult unsupported() {
    constexpr char kMessage[] = "bag source transfer callback unavailable";
    return contracts::error(kQolErrUnsupported, kMessage,
                            static_cast<uint32_t>(sizeof(kMessage) - 1));
}

QolResult invalid_argument() {
    constexpr char kMessage[] = "invalid bag transfer request";
    return contracts::error(kQolErrInvalidArgument, kMessage,
                            static_cast<uint32_t>(sizeof(kMessage) - 1));
}

bool valid_handle(contracts::QolItemHandle item) {
    return item.slot != 0 && item.generation != 0;
}

}  // namespace

QolResult export_item(int32_t space, contracts::QolItemHandle item, int32_t count,
                      BagTransfer* out) {
    if (out == nullptr || !valid_handle(item) || count <= 0) return invalid_argument();
    const contracts::QolBagSource* source = find_bag_source(space);
    if (source == nullptr || source->export_item == nullptr) return unsupported();
    return source->export_item(item, count, out);
}

QolResult import_item(const BagTransfer& transfer, int32_t target_space,
                      int32_t preferred_bag, int32_t preferred_slot,
                      contracts::QolBagMutationResult* out) {
    if (out == nullptr || transfer.category <= 0 || transfer.count <= 0 ||
        transfer.payload_size == 0 ||
        transfer.payload_size > contracts::kQolBagTransferPayloadMaxSize ||
        preferred_bag < -1 || preferred_slot < -1 ||
        (preferred_bag == -1 && preferred_slot != -1)) {
        return invalid_argument();
    }
    const contracts::QolBagSource* source = find_bag_source(target_space);
    if (source == nullptr || source->import_item == nullptr) return unsupported();
    return source->import_item(&transfer, preferred_bag, preferred_slot, out);
}

QolResult remove_item(int32_t space, contracts::QolItemHandle expected, int32_t count,
                      contracts::QolBagMutationResult* out) {
    if (out == nullptr || !valid_handle(expected) || count <= 0) return invalid_argument();
    const contracts::QolBagSource* source = find_bag_source(space);
    if (source == nullptr || source->remove == nullptr) return unsupported();
    return source->remove(expected, count, out);
}

QolResult locate(int32_t space, contracts::QolItemHandle item,
                 contracts::QolBagRef* out, int32_t* out_slot) {
    if (out == nullptr || out_slot == nullptr || !valid_handle(item)) return invalid_argument();
    const contracts::QolBagSource* source = find_bag_source(space);
    if (source == nullptr || source->identify == nullptr) return unsupported();
    const QolResult result = source->identify(item, out, out_slot);
    if (!contracts::succeeded(result)) return result;
    if (out->space_id != space || out->bag < 0 || *out_slot < 0) {
        constexpr char kMessage[] = "bag source returned invalid item location";
        return contracts::error(kQolErrInternal, kMessage,
                                static_cast<uint32_t>(sizeof(kMessage) - 1));
    }
    return result;
}

}  // namespace qol::features::core_item::bags::source_transport
