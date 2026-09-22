#pragma once

#include <type_traits>

#include "../result.h"
#include "ports.h"
#include "../version.h"

namespace qol::contracts {

struct QolSaveCallWriteApi {
    QolResult (*free_data)(void* data);
    QolResult (*encrypt)(void* data, int32_t size, int32_t mode, const char* key,
                         int32_t* out_result);
    QolResult (*mark_stale)();
};

struct QolSaveCallPort {
    QolVersion abi_version;
    QolSaveCallWriteApi write;
};

static_assert(std::is_standard_layout<QolSaveCallPort>::value,
              "QolSaveCallPort must remain standard layout");
static_assert(std::is_trivially_copyable<QolSaveCallPort>::value,
              "QolSaveCallPort must remain trivially copyable");

}  // namespace qol::contracts
