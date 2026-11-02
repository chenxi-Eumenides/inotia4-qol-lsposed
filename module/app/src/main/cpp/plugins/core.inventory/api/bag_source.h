#pragma once

#include "contracts/bag.h"

namespace qol::features::core_inventory {

extern const contracts::QolBagSourceRegistryApi kBagSourceRegistryApi;

const contracts::QolBagSource* find_bag_source(int32_t space_id);
uint32_t registered_bag_source_count();
const contracts::QolBagSource* registered_bag_source_at(uint32_t index);
void reset_bag_sources();

}  // namespace qol::features::core_inventory
