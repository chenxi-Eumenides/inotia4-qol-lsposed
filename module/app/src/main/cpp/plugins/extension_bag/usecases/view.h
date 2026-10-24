#pragma once

#include <string>

namespace qol::features::extension_bag::usecases {

std::string enter_view(int logical_bag);
std::string exit_view();
std::string select_bag(int logical_bag);

}  // namespace qol::features::extension_bag::usecases
