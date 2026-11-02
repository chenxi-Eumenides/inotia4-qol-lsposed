#pragma once

#include <cstdint>

#include "contracts/host_sdk.h"
#include "contracts/item_handle.h"
#include "plugins/extension_bag/model/item_handle_custody.h"
#include "plugins/extension_bag/model/virtual_bag_state.h"

namespace qol::features::extension_bag::api::detail {

extern const qol::contracts::QolHostSdk* g_sdk;
extern qol::features::extension_bag::model::ItemHandleCustody g_private_item_custody;
extern const virtual_bag::CountPolicy kCountPolicy;

const qol::contracts::QolItemHandleApi* item_handle_api();

}  // namespace qol::features::extension_bag::api::detail
