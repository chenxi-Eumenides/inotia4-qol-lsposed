#pragma once

#include "plugins/extension_bag/usecases/result.h"

namespace qol::features::extension_bag::usecases {

OpResult enter_view(int logical_bag);
OpResult exit_view();
OpResult select_bag(int logical_bag);

}  // namespace qol::features::extension_bag::usecases
