#pragma once

#include <cstdint>

#include "bag_catalog.h"

namespace qol::features::core_item::model {

enum class MoveRuleError : int32_t {
    kNone = 0,
    kInvalidSource,
    kInvalidTarget,
    kTaskBag,
    kInvalidCount,
    kInvalidSlot,
    kSameSlot,
};

struct MoveRequest {
    BagRef source;
    int32_t source_slot;
    BagRef target;
    int32_t target_slot;
    int32_t source_count;
    int32_t move_count;
    int32_t target_capacity;
};

MoveRuleError validate_move(const MoveRequest& request, const BagSpaceCounts& counts);

}  // namespace qol::features::core_item::model
