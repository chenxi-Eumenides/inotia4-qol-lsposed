#pragma once

#include <cstdint>
#include <string>

// 世界域（parse 域）：剧情状态 + 自研 BFS 路径/距离/调试路径。

bool data_story_active();
std::string data_story_json();
std::string data_path_json(int tx, int ty);
std::string data_distance_json(int32_t tx, int32_t ty);
std::string data_debug_path_json(int32_t tx, int32_t ty);

// 世界操作（移动/传送/交互）已迁 game/access/world.{h,cpp}（T2l-w-write）。

// 数据读取（read 域拆分）：位置/地图/瓦片/单位/掉落 JSON 构造。
void append_position(std::string& s, void* member);
std::string build_map_json();
std::string build_tiles_json();
std::string build_units_json();
std::string build_enemies_json();
std::string build_interactives_json();
std::string build_drops_json();
