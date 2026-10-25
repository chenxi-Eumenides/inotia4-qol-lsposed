#pragma once

#include "bag_source.h"

namespace qol::contracts {
struct QolInventoryExtensionProvider;
}

namespace qol::features::core_item::bags {

class ExtensionSourceBridge {
public:
    static void bind(const contracts::QolInventoryExtensionProvider* provider);
    static const contracts::QolBagSource* source();
};

}  // namespace qol::features::core_item::bags
