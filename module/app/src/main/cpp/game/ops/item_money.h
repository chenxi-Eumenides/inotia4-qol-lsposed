#pragma once

#include <cstdint>

#include "contracts/result.h"

namespace qol::game::ops {

QolResult set_money(int64_t money);
QolResult add_money(int64_t delta);
QolResult minus_money(int64_t delta);

}  // namespace qol::game::ops
