std::string data_path_json(int tx, int ty) {
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

std::string data_distance_json(int32_t tx, int32_t ty) {
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

std::string data_debug_path_json(int32_t tx, int32_t ty) {
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

// 当前 GAMESTATE_nState；g_gamestate 未解析视为 0（world）
