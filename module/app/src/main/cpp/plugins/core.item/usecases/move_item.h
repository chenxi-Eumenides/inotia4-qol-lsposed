#pragma once

#include "contracts/bag.h"

namespace qol::features::core_item::usecases {

QolResult move_item(const contracts::QolBagRef& from, int32_t from_slot, int32_t count,
                    const contracts::QolBagRef& to, int32_t to_slot,
                    contracts::QolBagMutationResult* out_target);

}  // namespace qol::features::core_item::usecases
