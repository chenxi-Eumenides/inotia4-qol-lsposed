#pragma once

#include <cstdint>
#include <string>

// 静态瓦片矩阵层（P0#瓦片矩阵，2026-08-12）：
// 从模块 assets 的 maps/tiles.json（Kotlin 读取后经 JNI 传入）加载 64×64 通行矩阵，
// 替代运行时读游戏内存 *(*(base+G_TILE_GOT_VMA))。
// bit3=阻挡(不可通行) bit6=显式阻挡 bit7=出口（data-sources.md §3.2 逆向）。
// mapId = MAPINFOBASE 记录下标（与 current_map_id() 一致，0-415）。

constexpr int STATIC_TILE_W = 64;
constexpr int STATIC_TILE_H = 64;
constexpr size_t STATIC_TILE_BYTES = STATIC_TILE_W * STATIC_TILE_H;  // 4096

// Kotlin 传入 tiles.json 全文（{"m0":{"mapId":0,"width":..,"height":..,"tiles":"<base64>"},...}），
// 解析并缓存全部地图矩阵。线程安全（初始化后只读）。
void set_static_tiles(const std::string& json);

// 按 mapId 取 4096B 矩阵；未加载/无该图返回 nullptr。
const uint8_t* static_tiles_for(int map_id);

// 静态数据是否已加载成功。
bool static_tiles_ready();

// 返回 mapId 对应的矩阵高度/宽度（用于 API 元数据；缺省 0）。
int static_tiles_width(int map_id);
int static_tiles_height(int map_id);
