#pragma once

#include <string>

// 缓存层：惰性/预取混合缓存（v0.4.59 表驱动）。对外 data_*_json 接口。

// host↔infra 内部注入面（A1 形态）：文档化的 slot 与 CacheSlot 表一致；仅 host 装配期调用。
// 只接收轻量函数指针，不反向依赖 host（infra 不 include host）。
void frame_cache_set_json_builder(int32_t slot, std::string (*fn)());

void frame_cache_start();
void frame_cache_stop();
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
