#pragma once

#include <cstdint>
#include <type_traits>

#include "../result.h"
#include "../version.h"

namespace qol::contracts {

struct QolCustomRecipeWritePort {
    QolVersion abi_version;
    QolResult (*write_stuff_need)(uint32_t index, int32_t category, int32_t count);
    QolResult (*set_stuff_item)(uint32_t index, void* item);
    QolResult (*description_set_off)();
};

static_assert(std::is_standard_layout<QolCustomRecipeWritePort>::value,
              "QolCustomRecipeWritePort must remain standard layout");
static_assert(std::is_trivially_copyable<QolCustomRecipeWritePort>::value,
              "QolCustomRecipeWritePort must remain trivially copyable");

}  // namespace qol::contracts
