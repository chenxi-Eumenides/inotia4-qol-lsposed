#pragma once

#include <cstdint>

#include "../item_handle.h"
#include "item_port.h"
#include "../result.h"
#include "../version.h"

namespace qol::contracts {

struct QolMoveMergePort {
    QolVersion abi_version;
    QolResult (*move_item)(QolItemHandle source, int32_t count, int32_t target_bag,
                           int32_t target_slot, int32_t* out_result);
    QolResult (*current_bag)(int32_t* out_bag);
    QolResult (*validate_native_item)(QolItemHandle item);
    QolResult (*swap_slots)(const QolBagRef* a, int32_t slot_a, const QolBagRef* b,
                            int32_t slot_b);
};

constexpr QolVersion kQolMoveMergePortVersion{1, 3};

}  // namespace qol::contracts
