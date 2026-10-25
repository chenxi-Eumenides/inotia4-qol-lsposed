#pragma once

#include <cstdint>
#include <vector>

#include "contracts/game_ports/item_port.h"

namespace qol::features::core_item::model {

using BagRef = contracts::QolBagRef;

struct RegisteredBagSpaceCount {
    int32_t space_id;
    int32_t count;
};

struct BagSpaceCounts {
    int32_t original_count = contracts::kQolOriginalBagCount;
    int32_t c18_count = 0;
    std::vector<RegisteredBagSpaceCount> registered;
};

int32_t space_count(const BagSpaceCounts& counts, int32_t space_id);
bool to_flat(const BagRef& ref, const BagSpaceCounts& counts, int32_t* out_flat);
bool from_flat(int32_t flat, const BagSpaceCounts& counts, BagRef* out);
bool valid_bag(const BagRef& ref, const BagSpaceCounts& counts);
bool is_task_bag(const BagRef& ref);
bool is_movable_source(const BagRef& ref);
bool is_valid_target(const BagRef& ref);

}  // namespace qol::features::core_item::model
