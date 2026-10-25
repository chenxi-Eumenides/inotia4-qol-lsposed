#pragma once

#include "contracts/item_handle.h"

namespace qol::features::core_item::model {

constexpr bool valid_item_handle(const contracts::QolItemHandle& handle) {
    return handle.slot != 0 && handle.generation != 0;
}

constexpr bool expected_handle_matches(const contracts::QolItemHandle& expected,
                                       const contracts::QolItemHandle& actual) {
    return valid_item_handle(expected) && valid_item_handle(actual) &&
           expected.slot == actual.slot && expected.generation == actual.generation;
}

}  // namespace qol::features::core_item::model
