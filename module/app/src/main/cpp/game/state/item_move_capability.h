#pragma once

#include "contracts/extension_bag.h"

namespace qol::game {

inline bool extension_bag_move_capability_ready(const contracts::ExtensionBagApi* api) {
    return api != nullptr && api->enabled != nullptr && api->enabled() &&
           api->move_item != nullptr;
}

}  // namespace qol::game
