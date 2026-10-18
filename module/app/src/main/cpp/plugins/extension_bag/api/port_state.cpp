#include "plugins/extension_bag/api/port_internal.h"

#include <cstddef>

namespace qol::features::extension_bag::api::detail {

const qol::contracts::QolHostSdk* g_sdk = nullptr;
qol::features::extension_bag::model::ItemHandleCustody g_private_item_custody;

namespace {

uint32_t canonical_count_from_field(uint32_t field) {
    return virtual_bag_canonical_from_field(field, field);
}

uint32_t view_count_from_canonical(uint32_t canonical, bool extended) {
    return virtual_bag_view_from_canonical(canonical, extended);
}

uint32_t write_view_to_field(uint32_t field, uint32_t count, bool extended) {
    return virtual_bag_field_from_canonical(field, count, extended);
}

uint32_t write_canonical_to_field(uint32_t field, uint32_t canonical) {
    return write_view_to_field(field, canonical, true);
}

uint32_t count_limit(bool extended) {
    return virtual_bag_count_limit(extended);
}

}  // namespace

const virtual_bag::CountPolicy kCountPolicy{
    &canonical_count_from_field,
    &view_count_from_canonical,
    &write_view_to_field,
    &write_canonical_to_field,
    &count_limit,
};

const qol::contracts::QolItemHandleApi* item_handle_api() {
    if (g_sdk == nullptr ||
        g_sdk->abi.size < offsetof(qol::contracts::QolHostSdk, item_handle) +
                              sizeof(qol::contracts::QolItemHandleApi)) return nullptr;
    return &g_sdk->item_handle;
}

}  // namespace qol::features::extension_bag::api::detail
