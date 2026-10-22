#pragma once

#include <cstdint>

#include "../../contracts/host_sdk.h"

namespace qol::features::custom_map {

using ResLoadToPoolFn = int32_t (*)(char* path, void** out_buf, int32_t* out_size);

void initialize(const contracts::QolHostSdk* sdk);
void clear_runtime();
void set_enabled(bool enabled);
bool enabled();
void set_original_loader(ResLoadToPoolFn loader);

int32_t load_resource(char* path, void** out_buf, int32_t* out_size);

}  // namespace qol::features::custom_map
