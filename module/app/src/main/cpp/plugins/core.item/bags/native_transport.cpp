#include "native_transport.h"

namespace qol::features::core_item::bags::native_transport {
namespace {

QolResult unavailable() {
    constexpr char kMessage[] = "host item transfer primitive unavailable";
    return contracts::error(kQolErrUnsupported, kMessage,
                            static_cast<uint32_t>(sizeof(kMessage) - 1));
}

bool is_native_space(int32_t space) {
    return space == contracts::kQolBagSpaceOriginal ||
           space == contracts::kQolBagSpaceC18;
}

}  // namespace

QolResult export_item(int32_t space, contracts::QolItemHandle item, int32_t count,
                      BagTransfer* out) {
    (void)space;
    (void)item;
    (void)count;
    (void)out;
    return unavailable();
}

QolResult import_item(const BagTransfer& transfer, int32_t target_space,
                      int32_t preferred_bag, int32_t preferred_slot,
                      contracts::QolBagMutationResult* out) {
    (void)transfer;
    (void)target_space;
    (void)preferred_bag;
    (void)preferred_slot;
    (void)out;
    return unavailable();
}

QolResult remove_item(int32_t space, contracts::QolItemHandle expected, int32_t count,
                      contracts::QolBagMutationResult* out) {
    (void)space;
    (void)expected;
    (void)count;
    (void)out;
    return unavailable();
}

QolResult locate(int32_t space, contracts::QolItemHandle item,
                 contracts::QolBagRef* out, int32_t* out_slot) {
    if (!is_native_space(space)) return unavailable();
    (void)item;
    (void)out;
    (void)out_slot;
    return unavailable();
}

}  // namespace qol::features::core_item::bags::native_transport
