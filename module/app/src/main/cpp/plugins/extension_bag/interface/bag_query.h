#pragma once

#include <string>

namespace qol::features::extension_bag {

std::string query_status_json();
bool query_bag_capacity(int logical_bag, int* out_capacity);
std::string query_inventory_fragment();

}  // namespace qol::features::extension_bag
