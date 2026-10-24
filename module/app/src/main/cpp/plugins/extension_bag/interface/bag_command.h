#pragma once

#include <cstdint>
#include <string>

namespace qol::features::extension_bag {

enum class BagCommandKind : int32_t {
    kEnterView = 0,
    kExitView = 1,
    kUnequip = 2,
    kSelectBag = 3,
    kClickItem = 4,
    kEquip = 5,
    kItem = 6,
    kSetBagCount = 7,
};

bool dispatch_bag_command(int32_t kind, int32_t a, int32_t b, int32_t c, int32_t d,
                          std::string* out);

}  // namespace qol::features::extension_bag
