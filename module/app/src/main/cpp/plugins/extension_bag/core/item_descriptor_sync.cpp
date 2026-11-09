#include "plugins/extension_bag/transaction/transaction.h"

#include <cstring>

#include "plugins/extension_bag/core/ui_state.h"
#include "plugins/extension_bag/model/virtual_bag_state.h"
#include "plugins/extension_bag/runtime/item_access.h"
#include "plugins/extension_bag/runtime/state.h"
#include "plugins/extension_bag/runtime/state_access.h"

bool sync_item_descriptor_from_object_locked(int bag, int slot, void* item, bool* out_changed) {
    if (out_changed != nullptr) *out_changed = false;
    if (!virtual_bag::valid_index(bag) || slot < 0 || slot >= virtual_bag::kSlotCount ||
        item == nullptr) {
        return false;
    }
    virtual_bag::Item& descriptor = ::qol::extension_bag::access::bag().items[bag][slot];
    if (descriptor.category <= 0 || descriptor.count <= 0) return false;

    std::array<uint8_t, virtual_bag::kSerializedItemBuffer> payload{};
    int payload_size = 0;
    if (!serialize_item_payload_locked(item, &payload, &payload_size) || payload_size <= 0) {
        return false;
    }
    // descriptor.count 恒为 canonical（R-38/R-48；getter 关闭态只返回 b 视图）。
    int canonical_count = descriptor.count;
    if (read_item_cumulate_count(item, &canonical_count)) {
        canonical_count = static_cast<int>(
            virtual_bag_canonical_count(item, static_cast<uint32_t>(descriptor.count)));
    }
    const bool changed =
        descriptor.payload_size != payload_size ||
        std::memcmp(descriptor.payload.data(), payload.data(), static_cast<size_t>(payload_size)) !=
            0 ||
        descriptor.count != canonical_count;
    if (!changed) return true;

    virtual_bag::Item updated = descriptor;
    updated.payload = payload;
    updated.payload_size = static_cast<uint16_t>(payload_size);
    updated.count = canonical_count;
    if (!virtual_bag::valid_payload(updated)) return false;
    descriptor = updated;
    qol::extension_bag::ui_state().objects.module_object_hashes[bag][slot] =
        virtual_bag::payload_hash(updated);
    if (out_changed != nullptr) *out_changed = true;
    return true;
}
