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

std::string build_map_json() {
    if (!game_in_world()) return "{\"error\":\"not in game\"}";
    std::string s = "{";
    s += "\"map_id\":" + std::to_string(current_map_id());
    append_position(s, lead_member());
        // 瓦片通行查询（P0#3：MAP_IsBlocking 反汇编确认，GOT G_TILE_GOT_VMA 双层解引用，y*64+x 索引，bit3=阻挡）
        // P0#瓦片矩阵（2026-08-12）：统一走 nav_tiles() —— 静态数据优先，缺失回退内存
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

// v0.4.62 P0：完整瓦片矩阵导出（64×64=4096B，base64 编码）
// 用途：P0 瓦片入静态数据采集——客户端一次性拿整图，无需 4096 次单 tile 查询
// P0#瓦片矩阵（2026-08-12）：静态数据就绪时返回静态矩阵（与运行时一致），否则回退内存
std::string build_tiles_json() {
    const uint8_t* tiles = nav_tiles();
    if (tiles == nullptr) return "{\"error\":\"no tiles\"}";
    static const char b64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    const size_t n = 64 * 64;
    std::string enc;
    enc.reserve((n + 2) / 3 * 4);
    for (size_t i = 0; i < n; i += 3) {
        uint32_t v = tiles[i] | (i + 1 < n ? uint32_t(tiles[i + 1]) << 8 : 0)
                                | (i + 2 < n ? uint32_t(tiles[i + 2]) << 16 : 0);
        enc += b64[(v >> 18) & 0x3F];
        enc += b64[(v >> 12) & 0x3F];
        enc += i + 1 < n ? b64[(v >> 6) & 0x3F] : '=';
        enc += i + 2 < n ? b64[v & 0x3F] : '=';
    }
    std::string s = "{\"map_id\":" + std::to_string(current_map_id());
    s += static_tiles_ready() ? ",\"src\":\"static\"" : ",\"src\":\"mem\"";
    s += ",\"size\":64,\"encoding\":\"base64\",\"tiles\":\"" + enc + "\"}";
    return s;
}

// units/enemies/interactives 共用构建器（v0.5.35 过滤下沉 native）：
// mode 0=全量（units，含 char_loc）1=敌人（enemies，type==1）2=可交互（interactives，type==2 且 interactable==true）
static std::string build_units_json_impl(int mode, bool include_charloc) {
    // CHARSYSTEM 角色对象池：*(G_CHAR_POOL_VMA) 指向英雄对象，对象按 C_OBJ_SIZE 步长连续排列
    // （frida 实测 2026-08-05：31 有效单位 = 3 队伍 + 怪物 + NPC，坐标与玩家同像素坐标系）。
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
                s += ",\"x\":" + std::to_string(x);
                s += ",\"y\":" + std::to_string(y);
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
                    s += ",\"x\":" + std::to_string(*reinterpret_cast<uint16_t*>(loc + 2));
                    s += ",\"y\":" + std::to_string(*reinterpret_cast<uint16_t*>(loc + 4)) + "}";
                }
                s += "]";
            }
        }
    }
    s += "}";
    return s;
}

std::string build_units_json() { return build_units_json_impl(0, true); }

std::string build_enemies_json() { return build_units_json_impl(1, false); }

std::string build_interactives_json() { return build_units_json_impl(2, false); }

std::string build_drops_json() {
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
