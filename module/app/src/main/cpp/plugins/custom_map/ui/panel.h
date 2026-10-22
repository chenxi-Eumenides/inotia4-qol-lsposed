#pragma once

#include <cstdint>

namespace qol::contracts {
struct QolHostSdk;
struct QolUiComponents;
}

namespace qol::features::custom_map {

void custom_map_panel_bind(const contracts::QolHostSdk* sdk,
                           const contracts::QolUiComponents* components);
bool custom_map_provider_open();
void custom_map_provider_close();
bool custom_map_provider_active();
const char* custom_map_provider_block_reason();
void custom_map_panel_enter();
void custom_map_panel_process();
void custom_map_panel_f3();
void custom_map_panel_f4();
uint32_t custom_map_panel_event(uint64_t event, uint64_t param, uint64_t param2);

}  // namespace qol::features::custom_map
