#include "plugins/extension_bag/extension_bag_port.h"
#include "plugins/extension_bag/extension_bag_bridge.h"
#include "plugins/extension_bag/extension_bag_context.h"
#include "plugins/extension_bag/game_ui_virtbag.h"
#include "plugins/extension_bag/model/virtual_bag_state.h"
#include "plugins/extension_bag/model/item_handle_custody.h"
#include "plugins/extension_bag/api/port_internal.h"
#include "contracts/inventory_extension.h"
#include "contracts/sell_price.h"
#include "contracts/host_sdk.h"

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <string>

using qol::features::extension_bag::api::detail::g_private_item_custody;
using qol::features::extension_bag::api::detail::g_sdk;
using qol::features::extension_bag::api::detail::item_handle_api;
using qol::features::extension_bag::api::detail::kCountPolicy;

void qol::features::extension_bag::extension_bag_bind_port_sdk(
    const qol::contracts::QolHostSdk* sdk) {
    g_sdk = sdk;
    virtual_bag::set_count_policy(sdk == nullptr ? nullptr : &kCountPolicy);
}

bool extension_bag_register_item_handle(void* item) {
    const auto* api = item_handle_api();
    if (item == nullptr || api == nullptr || api->register_item == nullptr) return false;
    qol::contracts::QolItemHandle handle{};
    const bool registered = api->register_item(
        item, qol::contracts::kQolItemHandleOwnerProvider, &handle) ==
        qol::contracts::kQolItemHandleStatusOk;
    if (registered && !g_private_item_custody.bind(item, handle)) {
        if (api->invalidate_item != nullptr) {
            (void)api->invalidate_item(item, qol::contracts::kQolItemHandleOwnerProvider);
        }
        return false;
    }
    return registered && handle.slot != 0 && handle.generation != 0;
}

void extension_bag_invalidate_item_handle(void* item) {
    const auto* api = item_handle_api();
    if (item == nullptr || api == nullptr || api->invalidate_item == nullptr) return;
    qol::contracts::QolItemHandle handle{};
    uint32_t owner = 0;
    if (api->lookup_item == nullptr || api->lookup_item(item, &handle, &owner) !=
                                          qol::contracts::kQolItemHandleStatusOk ||
        owner != qol::contracts::kQolItemHandleOwnerProvider ||
        !g_private_item_custody.begin_release(handle)) return;
    const int32_t status = api->invalidate_item(
        item, qol::contracts::kQolItemHandleOwnerProvider);
    (void)g_private_item_custody.finish_release(handle,
                                                 status == qol::contracts::kQolItemHandleStatusOk);
}

bool extension_bag_rebind_item_handle(void* item) {
    const auto* api = item_handle_api();
    if (item == nullptr || api == nullptr) return false;
    qol::contracts::QolItemHandle previous{};
    uint32_t previous_owner = 0;
    const bool had_previous = api->lookup_item != nullptr &&
        api->lookup_item(item, &previous, &previous_owner) ==
            qol::contracts::kQolItemHandleStatusOk;
    if (api->rebind_item != nullptr &&
        api->rebind_item(
            item, qol::contracts::kQolItemHandleOwnerProvider) ==
            qol::contracts::kQolItemHandleStatusOk) {
        qol::contracts::QolItemHandle rebound{};
        uint32_t owner = 0;
        if (api->lookup_item == nullptr || api->lookup_item(item, &rebound, &owner) !=
                                               qol::contracts::kQolItemHandleStatusOk ||
            owner != qol::contracts::kQolItemHandleOwnerProvider) return false;
        if (had_previous && previous_owner == qol::contracts::kQolItemHandleOwnerProvider) {
            if (g_private_item_custody.rebind(item, previous, rebound)) return true;
        }
        return g_private_item_custody.bind(item, rebound);
    }
    return extension_bag_register_item_handle(item);
}

bool extension_bag_private_item_for_handle(qol::contracts::QolItemHandle handle, void** out_item) {
    if (out_item == nullptr || handle.slot == 0 || handle.generation == 0) return false;
    *out_item = nullptr;
    if (!g_private_item_custody.lookup(handle, out_item)) return false;
    const auto* api = item_handle_api();
    qol::contracts::QolItemHandle current{};
    uint32_t owner = 0;
    if (api == nullptr || api->lookup_item == nullptr ||
        api->lookup_item(*out_item, &current, &owner) != qol::contracts::kQolItemHandleStatusOk ||
        owner != qol::contracts::kQolItemHandleOwnerProvider ||
        current.slot != handle.slot || current.generation != handle.generation) {
        (void)g_private_item_custody.begin_release(handle);
        (void)g_private_item_custody.finish_release(handle, true);
        *out_item = nullptr;
        return false;
    }
    return true;
}

bool extension_bag_sale_item_matches(qol::contracts::QolItemHandle expected, void* item,
                                     qol::contracts::QolItemHandle* out_current) {
    if (item == nullptr || out_current == nullptr || expected.slot == 0 ||
        expected.generation == 0) return false;
    void* custody_item = nullptr;
    if (!g_private_item_custody.lookup(expected, &custody_item) || custody_item != item)
        return false;
    const auto* api = item_handle_api();
    qol::contracts::QolItemHandle current{};
    uint32_t owner = 0;
    if (api == nullptr || api->lookup_item == nullptr ||
        api->lookup_item(item, &current, &owner) != qol::contracts::kQolItemHandleStatusOk ||
        owner != qol::contracts::kQolItemHandleOwnerProvider ||
        current.slot != expected.slot || current.generation != expected.generation) return false;
    *out_current = current;
    return true;
}

bool extension_bag_sale_item_begin_release(qol::contracts::QolItemHandle expected, void* item) {
    qol::contracts::QolItemHandle current{};
    return extension_bag_sale_item_matches(expected, item, &current) &&
           g_private_item_custody.begin_release(expected);
}

bool extension_bag_sale_item_finish_release(qol::contracts::QolItemHandle expected,
                                            bool committed) {
    return g_private_item_custody.finish_release(expected, committed);
}

bool extension_bag_item_handle_for_pointer(
    void* item, qol::contracts::QolItemHandle* out_handle) {
    const auto* api = item_handle_api();
    if (item == nullptr || out_handle == nullptr || api == nullptr) return false;
    uint32_t provider_id = 0;
    if (api->lookup_item(item, out_handle, &provider_id) ==
        qol::contracts::kQolItemHandleStatusOk) {
        if (provider_id == qol::contracts::kQolItemHandleOwnerProvider) {
            return g_private_item_custody.bind(item, *out_handle);
        }
        return true;
    }
    if (api->register_item(item, qol::contracts::kQolItemHandleOwnerGame, out_handle) !=
        qol::contracts::kQolItemHandleStatusOk) return false;
    return true;
}
