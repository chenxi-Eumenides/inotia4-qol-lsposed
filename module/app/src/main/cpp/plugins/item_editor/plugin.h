#pragma once

#include "../../contracts/plugin.h"
#include "../../contracts/game_ports/item_port.h"

namespace qol::features::item_editor {
inline constexpr uint32_t kResourceCount = 1;

// 面板 provider 声明数（PluginDescriptor.panel_provider_count）。
inline constexpr uint32_t kProviderCount = 1;
const contracts::PluginDescriptor& plugin_descriptor();
const contracts::QolHostSdk* host_sdk();
const contracts::QolGameHost* game_host();
const contracts::QolItemPort* item_port();
int32_t stack_limit_max_count();
void item_editor_equip_draw_wrapper();
void panel_enter();
void panel_process();
void panel_f3();
void panel_f4();
uint32_t panel_event(uint64_t event, uint64_t param, uint64_t param2);
bool provider_open();
void provider_close();
bool provider_active();
const char* provider_block_reason();
void panel_destroy();
bool panel_is_active();
bool item_editor_enabled();
void item_editor_force_disable();
}
