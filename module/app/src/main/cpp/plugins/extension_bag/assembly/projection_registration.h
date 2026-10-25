#pragma once

#include "contracts/result.h"

namespace qol::features::extension_bag {

QolResult register_projection_callbacks();
void unregister_projection_callbacks();

}  // namespace qol::features::extension_bag
