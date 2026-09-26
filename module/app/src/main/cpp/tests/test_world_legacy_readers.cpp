// T2l-w-read：world 读 8 函数 host 内联逐字等值（frozen 双跑）。
// host/legacy_json_builders.h 的 legacy_* 与下方 frozen_*（api/native 原文逐字复刻）在同一 fake
// 环境下逐字节对照；覆盖：非 world / world+g_base==0 / world 有内容（含 nav/or map/pool/drops）。
// frozen_* 由 api/native/game_world_readers.inc、game_world_navigation.inc 逐字生成（仅改名）。

#include "contracts/game_host.h"
#include "game/access/game_access.h"
#include "game/json/game_json.h"
#include "game/nav/game_nav.h"
#include "game/state/game_state.h"
#include "game/symbols/game_symbols.h"
#include "game_world.h"
#include "host/legacy_json_builders.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

static int g_pass = 0;
static int g_fail = 0;

#define CHECK(cond) do { \
    if (cond) ++g_pass; \
    else { ++g_fail; std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } \
} while (0)

uintptr_t g_base = 0;

static bool g_in_world = true;
static void* g_hero = nullptr;
static uint32_t g_fake_map_id = 34;
static uint8_t g_tiles[64 * 64]{};
static bool g_tiles_on = false;
static NavPath g_nav{};
static bool g_nav_ok = false;
static int g_depth = -1;
static bool g_depth_ok = false;

bool game_in_world() { return g_in_world; }
void* lead_member() { return g_hero; }
uint32_t current_map_id() { return g_fake_map_id; }
const uint8_t* nav_tiles() { return g_tiles_on ? g_tiles : nullptr; }
bool nav_blocked(const uint8_t* tiles, int tx, int ty) {
    return (tiles[ty * TILE_ROW_STRIDE + tx] & TILE_BLOCK_BIT) != 0;
}
bool nav_bfs(int, int, int, int, NavPath& out, bool, int, int, int) {
    if (!g_nav_ok) return false;
    out = g_nav;
    return true;
}
bool nav_bfs_multi(int, int, std::vector<int>& out) {
    out.assign(NAV_W * NAV_H, g_depth);
    return g_depth_ok;
}

// CHAR 池：0 号有效（英雄）；2 号有效（type=2 可交互）；其余无效。
static uint8_t g_pool[C_OBJ_SIZE * 3]{};
static uint8_t g_charloc[CHARLOC_SIZE]{};
static int8_t g_drop_count = 0;
static uint8_t g_drop_arr[GROUND_ITEM_SIZE * 2]{};
static uint8_t* g_drop_arr_ptr = g_drop_arr;
static uintptr_t g_region[1 << 14]{};  // 覆盖 CHARLOC/CHAR_POOL/DROP GOT 偏移跨度

bool pool_obj_valid(const uint8_t* obj) {
    return obj == g_pool || obj == g_pool + 2 * C_OBJ_SIZE;
}

GetNameFn fn_get_name = nullptr;
NpcSystemCheckFunctionDisplayFn fn_check_function_display = nullptr;

std::string json_escape(const char* s) {
    if (s == nullptr) return std::string();
    std::string out;
    for (const char* p = s; *p != '\0'; ++p) {
        if (*p == '"' || *p == '\\') out.push_back('\\');
        out.push_back(*p);
    }
    return out;
}

void append_position(std::string& s, void* member) {
    if (member != nullptr) {
        s += ",\"x\":" + std::to_string(
            *reinterpret_cast<int16_t*>(reinterpret_cast<uint8_t*>(member) + C_POS_X));
        s += ",\"y\":" + std::to_string(
            *reinterpret_cast<int16_t*>(reinterpret_cast<uint8_t*>(member) + C_POS_Y));
    } else {
        s += ",\"x\":-1,\"y\":-1";
    }
}

static char* fake_get_name(void*) {
    static char kName[] = "npc";
    return kName;
}
static int fake_check_function_display(int32_t fd) { return fd == 7 ? 1 : 2; }
namespace {

std::string frozen_build_map_json() {
    if (!game_in_world()) return "{\"error\":\"not in game\"}";
    std::string s = "{";
    s += "\"map_id\":" + std::to_string(current_map_id());
    append_position(s, lead_member());
        // 瓦片通行查询（P0#3：MAP_IsBlocking 反汇编确认，GOT G_TILE_GOT_VMA 双层解引用，y*64+x 索引，bit3=阻挡）
        // P0#瓦片矩阵：统一走 nav_tiles() —— 静态数据优先，缺失回退内存
        {
            const uint8_t* tiles = nav_tiles();
            if (tiles != nullptr) {
            uint8_t* lead = reinterpret_cast<uint8_t*>(lead_member());
            if (lead != nullptr) {
                int px = *reinterpret_cast<int16_t*>(lead + C_POS_X);
                int py = *reinterpret_cast<int16_t*>(lead + C_POS_Y);
                int tx = px >> 4, ty = py >> 4;
                if (tx >= 0 && ty >= 0) {
                    int idx = ty * TILE_ROW_STRIDE + tx;
                    s += ",\"tile\":{\"tx\":" + std::to_string(tx) + ",\"ty\":" + std::to_string(ty);
                    s += ",\"blocking\":" + std::string(((tiles[idx] & TILE_BLOCK_BIT) ? "true" : "false")) + "}";
                }
            }
            // v0.4.25 出口区域：扫描瓦片网格 bit7=1 的 tile（GAMEPLAY_CheckMapLink 同网格同索引）
            //   —— 出口供移动/切图使用（模块 move/walk 已按此触发 GoMapLink）
            {
                std::string ex;
                for (int yy = 0; yy < 64; ++yy) {
                    for (int xx = 0; xx < 64; ++xx) {
                        if (tiles[yy * TILE_ROW_STRIDE + xx] & 0x80) {
                            if (!ex.empty()) ex += ",";
                            ex += "{\"tx\":" + std::to_string(xx) + ",\"ty\":" + std::to_string(yy);
                            ex += ",\"px\":" + std::to_string(xx << 4) + ",\"py\":" + std::to_string(yy << 4) + "}";
                        }
                    }
                }
                s += ",\"exits\":[" + ex + "]";
            }
        }
    }
    s += "}";
    return s;
}

static std::string frozen_build_units_json_impl(int mode, bool include_charloc) {
    // CHARSYSTEM 角色对象池：*(G_CHAR_POOL_VMA) 指向英雄对象，对象按 C_OBJ_SIZE 步长连续排列
    // （frida 实测：31 有效单位 = 3 队伍 + 怪物 + NPC，坐标与玩家同像素坐标系）。
    // 有效性：type 0-2、status<=2、坐标 0-1500（未激活槽哨兵值 2048/16992/status>2，frida 实测排除）。
    // status: 0=队伍 1=城镇NPC/佣兵 2=怪物/召唤物。
    // type==2 为装饰/场景单位（路障/宝箱/火把/火堆等）。v0.4.25 曾过滤 type>1，本版取消过滤：
    // 输出 func_display（npc+0xa u16，NPCSYSTEM_CheckFunctionDisplay 入参）+ interactable（是否可交互），
    // 供消费者区分可交互装饰物（路障/宝箱）与纯装饰（火把）。
    std::string s = "{\"units\":[";
    if (g_base != 0) {
        uint8_t* pool = *reinterpret_cast<uint8_t**>(g_base + G_CHAR_POOL_VMA);
        if (pool != nullptr) {
            int emitted = 0;
            int hero_tx = -1, hero_ty = -1;
            void* hero = lead_member();
            if (hero != nullptr) {
                hero_tx = *reinterpret_cast<int16_t*>(reinterpret_cast<uint8_t*>(hero) + C_POS_X) >> 4;
                hero_ty = *reinterpret_cast<int16_t*>(reinterpret_cast<uint8_t*>(hero) + C_POS_Y) >> 4;
            }
            // v0.4.60 多目标 BFS：单次遍历得全图可达深度，单位查表 O(1)（替代每单位一次 BFS）
            std::vector<int> depth_map;
            bool bfs_ok = hero_tx >= 0 && nav_bfs_multi(hero_tx, hero_ty, depth_map);
            for (int i = 0; i < C_CHARSYSTEM_POOL_SLOTS; ++i) {
                uint8_t* obj = pool + i * C_OBJ_SIZE;
                if (!pool_obj_valid(obj)) continue;
                int16_t x = *reinterpret_cast<int16_t*>(obj + C_POS_X);
                int16_t y = *reinterpret_cast<int16_t*>(obj + C_POS_Y);
                int type = static_cast<int>(reinterpret_cast<int8_t*>(obj)[C_TYPE]);
                uint8_t status = obj[C_STATUS];
                if (mode == 1 && type != 1) continue;  // enemies：仅怪物/NPC
                if (mode == 2) {                        // interactives：仅可交互装饰物
                    if (type != 2) continue;
                    uint16_t fd = *reinterpret_cast<uint16_t*>(obj + C_FUNC_DISPLAY);
                    if (fn_check_function_display == nullptr || fn_check_function_display(fd) == 2) continue;
                }
                if (emitted > 0) s += ",";
                s += "{\"slot\":" + std::to_string(i);
                // API 坐标统一瓦片（B 方案）：像素 >> 4；snapshot/exits/teleport 同单位
                s += ",\"x\":" + std::to_string(x >> 4);
                s += ",\"y\":" + std::to_string(y >> 4);
                s += ",\"type\":" + std::to_string(type);
                s += ",\"status\":" + std::to_string(status);
                s += ",\"level\":" + std::to_string(static_cast<int>(reinterpret_cast<int8_t*>(obj)[C_LEVEL]));
                s += ",\"hp\":" + std::to_string(*reinterpret_cast<int32_t*>(obj + C_HP));
                s += ",\"mp\":" + std::to_string(*reinterpret_cast<int32_t*>(obj + C_MP));
                if (fn_get_name != nullptr) {
                    char* nm = fn_get_name(obj);
                    s += ",\"name\":" + (nm != nullptr ? "\"" + json_escape(nm) + "\"" : "null");
                } else {
                    s += ",\"name\":null";
                }
                if (type == 2) {
                    uint16_t fd = *reinterpret_cast<uint16_t*>(obj + C_FUNC_DISPLAY);
                    s += ",\"func_display\":" + std::to_string(fd);
                    if (fn_check_function_display != nullptr) {
                        s += ",\"interactable\":" +
                             std::string(fn_check_function_display(fd) != 2 ? "true" : "false");
                    }
                }
                if (bfs_ok) {
                    int utx = x >> 4, uty = y >> 4;
                    // 单位自身 tile 被 nav_unit_blocks 标记阻挡恒不可达，distance 取「能紧贴该对象的相邻可达格」最小路径长度
                    int best = -1;
                    const int ndx[4] = {1, -1, 0, 0};
                    const int ndy[4] = {0, 0, 1, -1};
                    for (int k = 0; k < 4; ++k) {
                        int nx = utx + ndx[k], ny = uty + ndy[k];
                        if (nx >= 0 && nx < NAV_W && ny >= 0 && ny < NAV_H) {
                            int d = depth_map[ny * NAV_W + nx];
                            if (d >= 0 && (best < 0 || d < best)) best = d;
                        }
                    }
                    if (best >= 0) {
                        s += ",\"distance\":" + std::to_string(best);
                    } else {
                        // 紧贴位置全部不可达：回退单次 BFS 取 nearestDistance
                        NavPath np;
                        if (nav_bfs(hero_tx, hero_ty, utx, uty, np)) {
                            s += ",\"distance\":-1,\"nearest_distance\":" + std::to_string(np.distance);
                        } else {
                            s += ",\"distance\":-1";
                        }
                    }
                }
                s += "}";
                ++emitted;
            }
        }
        s += "]";  // 闭合 units 数组
        if (include_charloc) {
            // CHARLOC 位置登记池（CHARLOCSYSTEM，10B/条：+0 f0, +2 x u16, +4 y u16）——P0#2 逆向产出
            // 注：+0 字段语义待确认（data-sources §2.6 标注为地图ID，CHARLOCSYSTEM_Add 反汇编为 a0）
            uint8_t* cl_pool = *reinterpret_cast<uint8_t**>(g_base + G_CHARLOC_POOL_VMA);
            uint16_t cl_count = *reinterpret_cast<uint16_t*>(g_base + G_CHARLOC_COUNT_VMA);
            if (cl_pool != nullptr && cl_count > 0 && cl_count <= 512) {
                s += ",\"char_loc\":[";
                for (int i = 0; i < cl_count; ++i) {
                    uint8_t* loc = cl_pool + i * CHARLOC_SIZE;
                    if (i > 0) s += ",";
                    s += "{\"f0\":" + std::to_string(static_cast<int>(loc[0]));
                    // API 坐标统一瓦片（B 方案）：像素 >> 4
                    s += ",\"x\":" + std::to_string(*reinterpret_cast<uint16_t*>(loc + 2) >> 4);
                    s += ",\"y\":" + std::to_string(*reinterpret_cast<uint16_t*>(loc + 4) >> 4) + "}";
                }
                s += "]";
            }
        }
    }
    s += "}";
    return s;
}

static std::string frozen_build_units_json() { return frozen_build_units_json_impl(0, true); }
static std::string frozen_build_enemies_json() { return frozen_build_units_json_impl(1, false); }
static std::string frozen_build_interactives_json() { return frozen_build_units_json_impl(2, false); }

std::string frozen_build_drops_json() {
    // 掉落物数组：G_DROP_COUNT_GOT_VMA 单层计数（int8）+ G_DROP_ARRAY_GOT_VMA 双层数组（0x20 步长）。
    // 实体：+0x0 掉落物对象指针 +0x8 x +0xa y +0x18 标志（bit1=拾取中）。
    std::string s = "{\"drops\":[";
    if (g_base != 0) {
        int8_t* cnt = *reinterpret_cast<int8_t**>(g_base + G_DROP_COUNT_GOT_VMA);
        int n = (cnt != nullptr) ? static_cast<int>(*cnt) : 0;
        uint8_t* arr = *reinterpret_cast<uint8_t**>(*reinterpret_cast<void**>(g_base + G_DROP_ARRAY_GOT_VMA));
        int emitted = 0;
        for (int i = 0; arr != nullptr && i < n; ++i) {
            uint8_t* e = arr + i * GROUND_ITEM_SIZE;
            int16_t x = *reinterpret_cast<int16_t*>(e + GROUND_ITEM_X);
            int16_t y = *reinterpret_cast<int16_t*>(e + GROUND_ITEM_Y);
            if (emitted > 0) s += ",";
            s += "{\"x\":" + std::to_string(x);
            s += ",\"y\":" + std::to_string(y) + "}";
            ++emitted;
        }
    }
    s += "]}";
    return s;
}

std::string frozen_data_path_json(int tx, int ty) {
    // v0.4.29 自研 BFS（替代 CHAR_SearchPath：游戏寻路不能绕远路）
    // 返回：inMap/found/distance/path（tile 中心像素）/nearest（不可达时最近可达点）
    if (!game_in_world()) return "{\"error\":\"not in game\"}";
    void* hero = lead_member();
    if (hero == nullptr) return "{\"error\":\"no player\"}";
    int px = *reinterpret_cast<int16_t*>(reinterpret_cast<uint8_t*>(hero) + C_POS_X);
    int py = *reinterpret_cast<int16_t*>(reinterpret_cast<uint8_t*>(hero) + C_POS_Y);
    std::string s = "{\"target\":{\"x\":" + std::to_string(tx) + ",\"y\":" + std::to_string(ty) + "}";
    s += ",\"start\":{\"x\":" + std::to_string(px) + ",\"y\":" + std::to_string(py) + "}";
    s += ",\"in_map\":" + std::string((tx >= 0 && tx < NAV_W * 16 && ty >= 0 && ty < NAV_H * 16) ? "true" : "false");
    NavPath np;
    if (!nav_bfs(px >> 4, py >> 4, tx >> 4, ty >> 4, np) || np.dir_count == 0) {
        s += ",\"found\":false,\"distance\":-1,\"nearest\":null,\"path\":[]}";
        return s;
    }
    s += ",\"found\":" + std::string(np.found ? "true" : "false");
    s += ",\"distance\":" + std::to_string(np.distance);
    if (np.nearest_x >= 0) {
        s += ",\"nearest\":{\"x\":" + std::to_string(np.nearest_x * 16 + 8) +
             ",\"y\":" + std::to_string(np.nearest_y * 16 + 8) +
             ",\"distance\":" + std::to_string(np.nearest_dist) + "}";
    } else {
        s += ",\"nearest\":null";
    }
    s += ",\"path\":[";
    int cx = px >> 4, cy = py >> 4;
    bool first = true;
    for (int i = 0; i < np.dir_count; ++i) {
        cx += NAV_DX[np.dirs[i]];
        cy += NAV_DY[np.dirs[i]];
        if (!first) s += ",";
        s += "{\"x\":" + std::to_string(cx * 16 + 8) + ",\"y\":" + std::to_string(cy * 16 + 8) + "}";
        first = false;
    }
    s += "]}";
    return s;
}

std::string frozen_data_distance_json(int32_t tx, int32_t ty) {
    // v0.4.29 玩家到目标的 BFS 最短距离（tile 步数）+ 可达性 + 最近可达点
    if (!game_in_world()) return "{\"error\":\"not in game\"}";
    void* hero = lead_member();
    if (hero == nullptr) return "{\"error\":\"no player\"}";
    int px = *reinterpret_cast<int16_t*>(reinterpret_cast<uint8_t*>(hero) + C_POS_X);
    int py = *reinterpret_cast<int16_t*>(reinterpret_cast<uint8_t*>(hero) + C_POS_Y);
    NavPath np;
    std::string s = "{\"target\":{\"x\":" + std::to_string(tx) + ",\"y\":" + std::to_string(ty) + "}";
    s += ",\"start\":{\"x\":" + std::to_string(px) + ",\"y\":" + std::to_string(py) + "}";
    if (!nav_bfs(px >> 4, py >> 4, tx >> 4, ty >> 4, np)) {
        s += ",\"found\":false,\"distance\":-1,\"nearest\":null}";
        return s;
    }
    s += ",\"found\":" + std::string(np.found ? "true" : "false");
    s += ",\"distance\":" + std::to_string(np.distance);
    if (np.nearest_x >= 0) {
        s += ",\"nearest\":{\"x\":" + std::to_string(np.nearest_x * 16 + 8) +
             ",\"y\":" + std::to_string(np.nearest_y * 16 + 8) +
             ",\"distance\":" + std::to_string(np.nearest_dist) + "}";
    } else {
        s += ",\"nearest\":null";
    }
    s += "}";
    return s;
}

std::string frozen_data_debug_path_json(int32_t tx, int32_t ty) {
    // GET /api/debug/path?tx=&ty=（tile 坐标）：debug 端点，直接调 nav_bfs 返回完整路线 + 阻挡信息。
    // 用途（P0 导航问题排查）：对比模块 BFS 判定 vs 引擎 CHAR_Move 实际碰撞、
    // 尸体阻挡影响（unit_blocks 含 hp=0 尸体）、重规划 resume 格（nearest）。
    if (!game_in_world()) return "{\"error\":\"not in game\"}";
    void* hero = lead_member();
    if (hero == nullptr) return "{\"error\":\"no player\"}";
    int px = *reinterpret_cast<int16_t*>(reinterpret_cast<uint8_t*>(hero) + C_POS_X);
    int py = *reinterpret_cast<int16_t*>(reinterpret_cast<uint8_t*>(hero) + C_POS_Y);
    int sx = px >> 4, sy = py >> 4;

    std::string s = "{\"start\":{\"tx\":" + std::to_string(sx) + ",\"ty\":" + std::to_string(sy) +
                    ",\"x\":" + std::to_string(px) + ",\"y\":" + std::to_string(py) + "}";
    s += ",\"target\":{\"tx\":" + std::to_string(tx) + ",\"ty\":" + std::to_string(ty) +
         ",\"x\":" + std::to_string(tx * 16 + 8) + ",\"y\":" + std::to_string(ty * 16 + 8) + "}";

    NavPath np;
    if (nav_bfs(sx, sy, tx, ty, np) && np.dir_count > 0) {
        s += ",\"found\":" + std::string(np.found ? "true" : "false");
        s += ",\"distance\":" + std::to_string(np.distance);
        s += ",\"path\":[";
        int cx = sx, cy = sy;
        bool first = true;
        for (int i = 0; i < np.dir_count; ++i) {
            cx += NAV_DX[np.dirs[i]];
            cy += NAV_DY[np.dirs[i]];
            if (!first) s += ",";
            s += "{\"tx\":" + std::to_string(cx) + ",\"ty\":" + std::to_string(cy) +
                 ",\"dir\":" + std::to_string(static_cast<int>(np.dirs[i])) + "}";
            first = false;
        }
        s += "]";
        if (np.nearest_x >= 0) {
            s += ",\"nearest\":{\"tx\":" + std::to_string(np.nearest_x) + ",\"ty\":" + std::to_string(np.nearest_y) +
                 ",\"distance\":" + std::to_string(np.nearest_dist) + "}";
        } else {
            s += ",\"nearest\":null";
        }
    } else {
        s += ",\"found\":false,\"distance\":-1,\"path\":[],\"nearest\":null";
    }

    // 单位阻挡列表（复用 nav_unit_blocks 过滤逻辑，含 hp 便于识别 hp=0 尸体阻挡）
    s += ",\"unit_blocks\":[";
    {
        bool first = true;
        if (g_base != 0) {
            uint8_t* pool = *reinterpret_cast<uint8_t**>(g_base + G_CHAR_POOL_VMA);
            if (pool != nullptr) {
                for (int i = 0; i < C_CHARSYSTEM_POOL_SLOTS; ++i) {
                    uint8_t* obj = pool + i * C_OBJ_SIZE;
                    if (obj[C_SITUATION] != 1) continue;
                    if (!pool_obj_valid(obj)) continue;
                    if (obj == hero) continue;
                    int16_t x = *reinterpret_cast<int16_t*>(obj + C_POS_X);
                    int16_t y = *reinterpret_cast<int16_t*>(obj + C_POS_Y);
                    int type = static_cast<int>(reinterpret_cast<int8_t*>(obj)[C_TYPE]);
                    int ux = x >> 4, uy = y >> 4;
                    if (ux < 0 || ux >= NAV_W || uy < 0 || uy >= NAV_H) continue;
                    if (!first) s += ",";
                    s += "{\"tx\":" + std::to_string(ux) + ",\"ty\":" + std::to_string(uy) +
                         ",\"slot\":" + std::to_string(i) +
                         ",\"type\":" + std::to_string(type) +
                          ",\"hp\":" + std::to_string(*reinterpret_cast<int32_t*>(obj + C_HP)) +
                          ",\"situation\":" + std::to_string(static_cast<int>(obj[C_SITUATION])) + "}";
                    first = false;
                }
            }
        }
    }
    s += "]";

    // 静态阻挡统计（全量 4096 tile 不输出，仅总数供参考）
    const uint8_t* tiles = nav_tiles();
    if (tiles != nullptr) {
        int blocked = 0;
        for (int i = 0; i < NAV_W * NAV_H; ++i) {
            if (nav_blocked(tiles, i % NAV_W, i / NAV_W)) ++blocked;
        }
        s += ",\"static_block_count\":" + std::to_string(blocked);
    }

    s += "}";
    return s;
}


static std::string frozen_data_quest_list_json() {
    if (!game_in_world()) return "{\"error\":\"not in game\"}";
    std::string s = "{\"quests\":[";
    if (g_base != 0) {
        uint8_t* cnt_ptr = *reinterpret_cast<uint8_t**>(g_base + G_QUEST_SLOT_COUNT_VMA);
        uint8_t* slots_ptr = *reinterpret_cast<uint8_t**>(g_base + G_QUEST_SLOTS_GOT_VMA);
        uint8_t*** st_got = reinterpret_cast<uint8_t***>(g_base + G_NPC_QUEST_STATE_GOT_VMA);
        uint8_t* states = (st_got != nullptr && *st_got != nullptr) ? **st_got : nullptr;
        if (cnt_ptr != nullptr && slots_ptr != nullptr) {
            uint8_t cnt = *cnt_ptr;
            uint8_t* slots = *reinterpret_cast<uint8_t**>(slots_ptr);
            if (slots != nullptr && cnt > 0 && cnt <= 20) {
                bool first = true;
                for (int i = 0; i < cnt; ++i) {
                    uint16_t qid = *reinterpret_cast<uint16_t*>(slots + i * 0xC);
                    if (!first) s += ",";
                    first = false;
                    int state = (states != nullptr) ? static_cast<int>(states[qid]) : -1;
                    bool deliv = (state == 2);
                    s += "{\"slot\":" + std::to_string(i) + ",\"quest_id\":" + std::to_string(qid) +
                         ",\"deliverable\":" + (deliv ? "true" : "false") + "}";
                }
            }
        }
    }
    s += "]}";
    return s;
}

static std::string frozen_data_quest_completed_json() {
    if (!game_in_world()) return "{\"error\":\"not in game\"}";
    std::string s = "{\"quests\":[";
    if (g_base != 0) {
        uint16_t* qcnt_var = *reinterpret_cast<uint16_t**>(g_base + G_QUEST_COUNT_GOT_VMA);
        uint8_t*** st_got = reinterpret_cast<uint8_t***>(g_base + G_NPC_QUEST_STATE_GOT_VMA);
        if (qcnt_var != nullptr && st_got != nullptr && *st_got != nullptr && **st_got != nullptr) {
            uint16_t qcnt = *qcnt_var;
            uint8_t* states = **st_got;
            bool first = true;
            if (qcnt > 0 && qcnt <= 512) {
                for (uint16_t qid = 0; qid < qcnt; ++qid) {
                    if (states[qid] != 3) continue;
                    if (!first) s += ",";
                    first = false;
                    s += "{\"quest_id\":" + std::to_string(qid) + "}";
                }
            }
        }
    }
    s += "]}";
    return s;
}

static std::string frozen_data_quest_active_json() {
    if (!game_in_world()) return "{\"error\":\"not in game\"}";
    std::string s = "{\"quests\":[";
    if (g_base != 0) {
        uint8_t* cnt_ptr = *reinterpret_cast<uint8_t**>(g_base + G_QUEST_SLOT_COUNT_VMA);
        uint8_t* slots_ptr = *reinterpret_cast<uint8_t**>(g_base + G_QUEST_SLOTS_GOT_VMA);
        uint8_t*** st_got = reinterpret_cast<uint8_t***>(g_base + G_NPC_QUEST_STATE_GOT_VMA);
        uint8_t* states = (st_got != nullptr && *st_got != nullptr) ? **st_got : nullptr;
        if (cnt_ptr != nullptr && slots_ptr != nullptr && cnt_ptr != slots_ptr) {
            uint8_t cnt = *cnt_ptr;
            uint8_t* slots = *reinterpret_cast<uint8_t**>(slots_ptr);
            if (slots != nullptr && cnt > 0 && cnt <= 20) {
                bool first = true;
                for (int i = 0; i < cnt; ++i) {
                    uint16_t qid = *reinterpret_cast<uint16_t*>(slots + i * 0xC);
                    if (!first) s += ",";
                    first = false;
                    int state = (states != nullptr) ? static_cast<int>(states[qid]) : -1;
                    bool deliv = (state == 2);
                    s += "{\"slot\":" + std::to_string(i) + ",\"quest_id\":" + std::to_string(qid) +
                         ",\"deliverable\":" + (deliv ? "true" : "false") + "}";
                }
            }
        }
    }
    s += "]}";
    return s;
}

}  // namespace
// ---- fixtures ----

static uint8_t g_hero_obj[C_OBJ_SIZE]{};

static void reset_env() {
    g_base = 0;
    g_in_world = true;
    g_hero = nullptr;
    g_fake_map_id = 34;
    std::memset(g_tiles, 0, sizeof(g_tiles));
    g_tiles_on = false;
    g_nav = NavPath{};
    g_nav_ok = false;
    g_depth = -1;
    g_depth_ok = false;
    std::memset(g_pool, 0, sizeof(g_pool));
    std::memset(g_region, 0, sizeof(g_region));
    std::memset(g_hero_obj, 0, sizeof(g_hero_obj));
    g_drop_count = 0;
    std::memset(g_drop_arr, 0, sizeof(g_drop_arr));
    fn_get_name = nullptr;
    fn_check_function_display = nullptr;
}

static void check_all_equal(const char* tag) {
    (void)tag;
    CHECK(qol::host::legacy_build_map_json() == frozen_build_map_json());
    CHECK(qol::host::legacy_build_units_json() == frozen_build_units_json());
    CHECK(qol::host::legacy_build_enemies_json() == frozen_build_enemies_json());
    CHECK(qol::host::legacy_build_interactives_json() == frozen_build_interactives_json());
    CHECK(qol::host::legacy_build_drops_json() == frozen_build_drops_json());
    CHECK(qol::host::legacy_data_path_json(19, 8) == frozen_data_path_json(19, 8));
    CHECK(qol::host::legacy_data_distance_json(19, 8) == frozen_data_distance_json(19, 8));
    CHECK(qol::host::legacy_data_debug_path_json(19, 8) == frozen_data_debug_path_json(19, 8));
}

static uint8_t g_quest_count = 0;
static uint8_t g_quest_slots[2 * 0xC]{};
static uint8_t* g_quest_slots_pp = g_quest_slots;
static uint8_t g_quest_state_tbl[16]{};
static uint8_t* g_quest_states = g_quest_state_tbl;
static uint8_t* g_quest_states_pp = g_quest_state_tbl;
static uint16_t g_quest_total = 0;

static void quest_bind_got() {
    g_base = reinterpret_cast<uintptr_t>(g_region) - G_QUEST_SLOT_COUNT_VMA;
    *reinterpret_cast<uint8_t**>(g_base + G_QUEST_SLOT_COUNT_VMA) = &g_quest_count;
    *reinterpret_cast<uint8_t**>(g_base + G_QUEST_SLOTS_GOT_VMA) =
        reinterpret_cast<uint8_t*>(&g_quest_slots_pp);
    *reinterpret_cast<uint8_t**>(g_base + G_NPC_QUEST_STATE_GOT_VMA) =
        reinterpret_cast<uint8_t*>(&g_quest_states_pp);
    *reinterpret_cast<uint8_t**>(g_base + G_QUEST_COUNT_GOT_VMA) =
        reinterpret_cast<uint8_t*>(&g_quest_total);
}

static void check_quest_equal(const char* tag) {
    (void)tag;
    CHECK(qol::host::legacy_data_quest_list_json() == frozen_data_quest_list_json());
    CHECK(qol::host::legacy_data_quest_completed_json() == frozen_data_quest_completed_json());
    CHECK(qol::host::legacy_data_quest_active_json() == frozen_data_quest_active_json());
}

static void test_quest_reads() {
    // F4：有任务内容（2 槽；state[7]=2 可交付、state[9]=1；完成表 state[3]=3）。
    reset_env();
    g_quest_count = 2;
    *reinterpret_cast<uint16_t*>(g_quest_slots + 0) = 7;
    *reinterpret_cast<uint16_t*>(g_quest_slots + 0xC) = 9;
    std::memset(g_quest_state_tbl, 0, sizeof(g_quest_state_tbl));
    g_quest_state_tbl[7] = 2;
    g_quest_state_tbl[9] = 1;
    g_quest_state_tbl[3] = 3;
    g_quest_states = g_quest_state_tbl;
    g_quest_states_pp = g_quest_state_tbl;
    g_quest_total = 10;
    quest_bind_got();
    check_quest_equal("F4");
    CHECK(qol::host::legacy_data_quest_list_json().find("\"quest_id\":7,\"deliverable\":true") !=
          std::string::npos);
    CHECK(qol::host::legacy_data_quest_active_json().find("\"slot\":1,\"quest_id\":9") !=
          std::string::npos);
    CHECK(qol::host::legacy_data_quest_completed_json() == "{\"quests\":[{\"quest_id\":3}]}");

    // F4b：状态表缺失 → deliverable 恒 false（list/active 的 states==nullptr 分支）。
    g_quest_states_pp = nullptr;
    check_quest_equal("F4b");
    CHECK(qol::host::legacy_data_quest_list_json().find("\"deliverable\":false") !=
          std::string::npos);

    // F5：g_base==0 → 三读空数组。
    reset_env();
    check_quest_equal("F5");
    CHECK(qol::host::legacy_data_quest_list_json() == "{\"quests\":[]}");
    CHECK(qol::host::legacy_data_quest_completed_json() == "{\"quests\":[]}");
    CHECK(qol::host::legacy_data_quest_active_json() == "{\"quests\":[]}");

    // F6：非 world → 错误短体。
    reset_env();
    g_in_world = false;
    check_quest_equal("F6");
    CHECK(qol::host::legacy_data_quest_active_json() == "{\"error\":\"not in game\"}");
}

int main() {
    // F1：world、g_base==0、无主角 → 空面 + "no player" 短体。
    reset_env();
    check_all_equal("F1");
    CHECK(qol::host::legacy_build_map_json() == "{\"map_id\":34,\"x\":-1,\"y\":-1}");
    CHECK(qol::host::legacy_build_units_json() == "{\"units\":[}");  // g_base==0 时不闭合数组（legacy 形状，逐字保留）
    CHECK(qol::host::legacy_build_drops_json() == "{\"drops\":[]}");
    CHECK(qol::host::legacy_data_path_json(19, 8) == "{\"error\":\"no player\"}");

    // F2：非 world → 错误短体（units/drops 无 world 短路，仍走 g_base==0 空面）。
    reset_env();
    g_in_world = false;
    check_all_equal("F2");
    CHECK(qol::host::legacy_build_map_json() == "{\"error\":\"not in game\"}");
    CHECK(qol::host::legacy_data_distance_json(19, 8) == "{\"error\":\"not in game\"}");
    CHECK(qol::host::legacy_build_units_json() == "{\"units\":[}");  // g_base==0 时不闭合数组（legacy 形状，逐字保留）
    CHECK(qol::host::legacy_build_drops_json() == "{\"drops\":[]}");

    // F3：world 有内容（hero/瓦片/出口/pool/charloc/drops/nav）。
    reset_env();
    g_hero = g_hero_obj;
    reinterpret_cast<int16_t*>(g_hero_obj + C_POS_X)[0] = 0x20;  // tile (2,3)
    reinterpret_cast<int16_t*>(g_hero_obj + C_POS_Y)[0] = 0x30;
    g_tiles_on = true;
    g_tiles[3 * TILE_ROW_STRIDE + 2] |= TILE_BLOCK_BIT;  // 主角所在格阻挡
    g_tiles[1 * TILE_ROW_STRIDE + 1] |= 0x80;            // 出口格
    g_nav.found = true;
    g_nav.distance = 5;
    g_nav.dirs[0] = 3;
    g_nav.dirs[1] = 3;
    g_nav.dir_count = 2;
    g_nav.nearest_x = 4;
    g_nav.nearest_y = 5;
    g_nav.nearest_dist = 2;
    g_nav_ok = true;
    g_depth = 7;
    g_depth_ok = true;
    // pool：0 号（type0/status0）与 2 号（type2/func_display 7 → interactable）有效。
    {
        uint8_t* o0 = g_pool;
        uint8_t* o2 = g_pool + 2 * C_OBJ_SIZE;
        o0[C_TYPE] = 0;
        o0[C_STATUS] = 0;
        o0[C_LEVEL] = 10;
        o0[C_SITUATION] = 1;
        *(int16_t*)(o0 + C_POS_X) = 0x20;
        *(int16_t*)(o0 + C_POS_Y) = 0x30;
        *(int32_t*)(o0 + C_HP) = 100;
        *(int32_t*)(o0 + C_MP) = 50;
        o2[C_TYPE] = 2;
        o2[C_STATUS] = 1;
        o2[C_LEVEL] = 3;
        o2[C_SITUATION] = 1;
        *(int16_t*)(o2 + C_POS_X) = 0x40;
        *(int16_t*)(o2 + C_POS_Y) = 0x50;
        *(int32_t*)(o2 + C_HP) = 5;
        *(uint16_t*)(o2 + C_FUNC_DISPLAY) = 7;
    }
    g_charloc[0] = 3;
    *(uint16_t*)(g_charloc + LOC_POS_X) = 0x20;
    *(uint16_t*)(g_charloc + LOC_POS_Y) = 0x40;
    g_drop_count = 1;
    *(int16_t*)(g_drop_arr + GROUND_ITEM_X) = 100;
    *(int16_t*)(g_drop_arr + GROUND_ITEM_Y) = 200;
    fn_get_name = &fake_get_name;
    fn_check_function_display = &fake_check_function_display;
    g_base = reinterpret_cast<uintptr_t>(g_region) - G_DROP_COUNT_GOT_VMA;
    *reinterpret_cast<uint8_t**>(g_base + G_DROP_COUNT_GOT_VMA) =
        reinterpret_cast<uint8_t*>(&g_drop_count);
    *reinterpret_cast<uint8_t**>(g_base + G_DROP_ARRAY_GOT_VMA) =
        reinterpret_cast<uint8_t*>(&g_drop_arr_ptr);
    *reinterpret_cast<uint8_t**>(g_base + G_CHARLOC_POOL_VMA) = g_charloc;
    *reinterpret_cast<uint16_t*>(g_base + G_CHARLOC_COUNT_VMA) = 1;
    *reinterpret_cast<uint8_t**>(g_base + G_CHAR_POOL_VMA) = g_pool;
    check_all_equal("F3");
    CHECK(qol::host::legacy_build_map_json().find("\"blocking\":true") != std::string::npos);
    CHECK(qol::host::legacy_build_map_json().find("\"px\":16,\"py\":16") != std::string::npos);
    CHECK(qol::host::legacy_build_units_json().find("\"char_loc\"") != std::string::npos);
    CHECK(qol::host::legacy_build_units_json().find("\"slot\":2") != std::string::npos);
    CHECK(qol::host::legacy_build_enemies_json().find("\"units\":[]") != std::string::npos);
    CHECK(qol::host::legacy_build_interactives_json().find("\"interactable\":true") !=
          std::string::npos);
    CHECK(qol::host::legacy_build_drops_json().find("{\"x\":100,\"y\":200}") != std::string::npos);
    CHECK(qol::host::legacy_data_path_json(19, 8).find("\"found\":true") != std::string::npos);
    CHECK(qol::host::legacy_data_distance_json(19, 8).find("\"nearest\":{\"x\":72,\"y\":88") !=
          std::string::npos);
    CHECK(qol::host::legacy_data_debug_path_json(19, 8).find("\"static_block_count\":") !=
          std::string::npos);

    test_quest_reads();

    std::printf("world_legacy_readers: pass=%d fail=%d\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
