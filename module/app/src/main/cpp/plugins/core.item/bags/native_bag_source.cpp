#include "native_bag_source.h"

namespace qol::features::core_item::bags {
namespace {

QolResult unsupported() {
    constexpr char kMessage[] = "native bag read unavailable";
    return contracts::error(kQolErrUnsupported, kMessage,
                            static_cast<uint32_t>(sizeof(kMessage) - 1));
}

QolResult invalid_argument() {
    constexpr char kMessage[] = "invalid native bag request";
    return contracts::error(kQolErrInvalidArgument, kMessage,
                            static_cast<uint32_t>(sizeof(kMessage) - 1));
}

}  // namespace

NativeBagSource::NativeBagSource(const contracts::QolItemPort* port) : port_(port) {}

bool NativeBagSource::supports_space(int32_t space_id) const {
    return space_id == contracts::kQolBagSpaceOriginal ||
           space_id == contracts::kQolBagSpaceC18;
}

int32_t NativeBagSource::count(int32_t space_id) const {
    if (!supports_space(space_id) || port_ == nullptr ||
        !contracts::qol_item_port_bag_space_size_valid(port_) ||
        port_->read.bag_count == nullptr) {
        return 0;
    }
    int32_t result = 0;
    if (!contracts::succeeded(port_->read.bag_count(space_id, &result)) || result < 0) return 0;
    return result;
}

QolResult NativeBagSource::info(const contracts::QolBagRef& bag,
                                contracts::QolBagInfo* out) const {
    if (out == nullptr || bag.bag < 0 || !supports_space(bag.space_id)) return invalid_argument();
    if (port_ == nullptr || !contracts::qol_item_port_bag_space_size_valid(port_) ||
        port_->read.bag_info == nullptr) {
        return unsupported();
    }
    return port_->read.bag_info(&bag, out);
}

QolResult NativeBagSource::item_at(const contracts::QolBagRef& bag, int32_t slot,
                                   contracts::QolInventoryItemRef* out) const {
    if (out == nullptr || bag.bag < 0 || slot < 0 || !supports_space(bag.space_id)) {
        return invalid_argument();
    }
    if (port_ == nullptr || !contracts::qol_item_port_bag_space_size_valid(port_) ||
        port_->read.item_at == nullptr) {
        return unsupported();
    }
    return port_->read.item_at(&bag, slot, out);
}

QolResult NativeBagSource::for_each(int32_t space_id,
                                    contracts::QolInventoryBagSlotFn fn, void* ctx) const {
    if (!supports_space(space_id) || fn == nullptr) return invalid_argument();
    if (port_ == nullptr || !contracts::qol_item_port_bag_space_size_valid(port_) ||
        port_->read.for_each_space_slot == nullptr) {
        return unsupported();
    }
    return port_->read.for_each_space_slot(space_id, fn, ctx);
}

}  // namespace qol::features::core_item::bags
