#pragma once

#include "plugins/extension_bag/usecases/result.h"

namespace qol::features::extension_bag::usecases {

ClickItemResult click_item(int logical_bag, int slot);

}  // namespace qol::features::extension_bag::usecases
