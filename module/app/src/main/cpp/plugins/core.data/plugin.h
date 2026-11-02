#pragma once

#include "contracts/plugin.h"

namespace qol::features::core_data {

const contracts::PluginDescriptor& plugin_descriptor();

QolResult begin_asset_import(uint32_t expected_files, uint32_t expected_bytes);
QolResult import_asset(const char* path, uint32_t path_size, const uint8_t* bytes,
                       uint32_t byte_size);
QolResult finish_asset_import();
void cancel_asset_import();

}  // namespace qol::features::core_data
