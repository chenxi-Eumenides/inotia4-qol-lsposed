#pragma once

#include <cstdint>

#include "../item_handle.h"
#include "../result.h"
#include "../version.h"

namespace qol::contracts {

struct QolMoveMergePort {
    QolVersion abi_version;
    QolResult (*move_item)(QolItemHandle source, int32_t count, int32_t target_bag,
                           int32_t target_slot, int32_t* out_result);
    QolResult (*current_bag)(int32_t* out_bag);
    QolResult (*validate_native_item)(QolItemHandle item);
};

constexpr QolVersion kQolMoveMergePortVersion{1, 2};

}  // namespace qol::contracts
