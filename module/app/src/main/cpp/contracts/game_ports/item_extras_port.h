#pragma once

#include <cstdint>

#include "../result.h"
#include "../version.h"

namespace qol::contracts {

struct QolItemExtrasReadApi {
    QolResult (*ability_level)(const void* item, int32_t* out_level);
};

struct QolItemExtrasPort {
    QolVersion abi_version;
    QolItemExtrasReadApi read;
};

}  // namespace qol::contracts
