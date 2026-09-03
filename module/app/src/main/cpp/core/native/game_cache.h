#pragma once

#include <string>

// 缓存层：惰性/预取混合缓存（v0.4.59 表驱动）。对外 data_*_json 接口。

void frame_cache_start();
void frame_cache_force_refresh();
bool frame_cache_ready();

std::string data_player_json();
std::string data_party_json();
std::string data_inventory_json();
std::string data_map_json();
std::string data_units_json();
std::string data_gamestate_json();
std::string data_snapshot_json();
std::string data_skills_json();
std::string data_mercenaries_json();
std::string data_drops_json();
std::string data_enemies_json();
std::string data_interactives_json();
