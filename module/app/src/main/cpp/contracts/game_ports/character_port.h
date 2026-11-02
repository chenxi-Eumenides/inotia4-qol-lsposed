#pragma once

#include <cstdint>

#include "../result.h"
#include "../version.h"

namespace qol::contracts {

struct QolCharacterPortReadApi {
    QolResult (*type)(const void* character, int32_t* out_type);
    QolResult (*is_active_player_group)(const void* character, int32_t* out_active);
};

struct QolCharacterPort {
    QolVersion abi_version;
    QolCharacterPortReadApi read;
};

}  // namespace qol::contracts
