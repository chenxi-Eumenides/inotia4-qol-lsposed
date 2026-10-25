#pragma once

#include "contracts/item_handle.h"
#include "contracts/result.h"

namespace qol::features::core_item::bags {

QolResult recheck_expected_handle(const contracts::QolItemHandle& expected,
                                  const contracts::QolItemHandle& current);

}  // namespace qol::features::core_item::bags
