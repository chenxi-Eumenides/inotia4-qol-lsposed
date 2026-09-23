std::string data_op_teleport(int32_t map_id, int32_t x, int32_t y, bool force) {
    if (!game_in_world()) return op_err("not in game");
    if (const char* ui = ui_blocked()) {
        std::string err = "ui occupied: ";
        err += ui;
        return op_err(err.c_str());
    }
    if (member_or_null(0) == nullptr) return op_err("world not ready");
    if (fn_mapchange_set == nullptr || fn_gamestate_set_state == nullptr)
        return op_err("symbol not resolved");
    const bool same_map = map_id <= 0;
    const int target = same_map ? static_cast<int>(current_map_id()) : map_id;
    // API 坐标统一瓦片（B 方案）：x/y 全端点为瓦片索引，map_id=0 表示当前图（同图重入）。
    // MAPCHANGE_Set 的 x/y 是瓦片索引，引擎本身不校验地界，越界落点会把角色放到地图数据
    // 之外（真机表现为角色不出现、不可操作，需重进档恢复；曾以 tile(200,200) 落在 34×38
    // 的图外复现）。地界取静态数据该图 width/height（tiles.json 反汇编口径：地图 tile
    // 网格；像素 = tile*16 + 8），图内再查阻挡位 TILE_BLOCK_BIT（与寻路 nav_blocked 同口径）。
    // 目标图静态矩阵缺失时无法校验，放行。
    const int tx = x;
    const int ty = y;
    if (!force) {
        const int tw = static_tiles_width(target);
        const int th = static_tiles_height(target);
        if (tw > 0 && th > 0 && (tx < 0 || tx >= tw || ty < 0 || ty >= th)) {
            const std::string msg = "tile (" + std::to_string(tx) + "," + std::to_string(ty) +
                                    ") out of map bounds " + std::to_string(tw) + "x" +
                                    std::to_string(th) + " (force to override)";
            return op_err(msg.c_str());
        }
        if (tx >= 0 && tx < NAV_W && ty >= 0 && ty < NAV_H) {
            const uint8_t* tiles = static_tiles_for(target);
            if (tiles != nullptr && (tiles[ty * NAV_W + tx] & TILE_BLOCK_BIT) != 0) {
                const std::string msg = "tile (" + std::to_string(tx) + "," + std::to_string(ty) +
                                        ") not walkable (force to override)";
                return op_err(msg.c_str());
            }
        }
    }
    // 原版切图只发生在世界态稳定后，且发起点全部在游戏主线程（走出口 tile 的
    // GoMapLinkByChar / 世界传送）。API 曾在 HTTP worker 线程直调 fn_change_map，与
    // 渲染并发导致 SPR_Draw SIGSEGV（真机 tombstone 两次复现）——与 main_menu/enter_slot
    // 同机理，切图发起必须经 transition_run 排到游戏主线程逻辑帧点执行。
    // 发起链复刻真机验证过的世界传送功能（plugins/world_teleport）：MAPCHANGE_Set
    // 写目标图与落点 + GAMESTATE_SetState(MAP_CHANGE) 把控制权交给官方切图状态机
    // （清理/加载/定位/镜头/恢复控制全部由状态机完成）。直调 fn_change_map 或
    // SET_POSITION 绕过状态机的方案均已真机否决：前者镜头体系断裂（传送后手动移动
    // 镜头不跟随，仅重进档恢复），后者同图瞬移破坏镜头跟随且无状态重置。
    // map_id=0 表示当前图（同图重入）；落点换算后为 tile；方向保持主控当前朝向。
    std::string result;
    if (!transition_run([target, tx, ty](std::string* out) {
            if (gamestate_state() != 0) {
                *out = op_err("map transition in progress");
                return;
            }
            void* ch = member_or_null(0);
            if (ch == nullptr) {
                *out = op_err("world not ready");
                return;
            }
            const int direction =
                *reinterpret_cast<uint8_t*>(reinterpret_cast<uint8_t*>(ch) + C_DIRECTION);
            fn_mapchange_set(target, tx, ty, direction);
            fn_gamestate_set_state(GAMESTATE_MAP_CHANGE);
            *out = op_ok();
        }, kTransitionTimeoutMs, &result)) {
        return op_err(result.empty() ? "transition timeout" : result.c_str());
    }
    return result;
}
// owner 命令内联入口：命令排空本身就在 kFramePointLogicPre 相位，不能再经 transition_run
// 跨帧派发并阻塞等待自己。校验与转换体与 data_op_teleport 保持一致（同校验语义、同发起链）；
// 传送结果是原始 op JSON（不读转场后的世界态），故无需延迟结算。
std::string data_op_teleport_direct(int32_t map_id, int32_t x, int32_t y, bool force) {
    if (!game_in_world()) return op_err("not in game");
    if (const char* ui = ui_blocked()) {
        std::string err = "ui occupied: ";
        err += ui;
        return op_err(err.c_str());
    }
    if (member_or_null(0) == nullptr) return op_err("world not ready");
    if (fn_mapchange_set == nullptr || fn_gamestate_set_state == nullptr)
        return op_err("symbol not resolved");
    const bool same_map = map_id <= 0;
    const int target = same_map ? static_cast<int>(current_map_id()) : map_id;
    const int tx = x;
    const int ty = y;
    if (!force) {
        const int tw = static_tiles_width(target);
        const int th = static_tiles_height(target);
        if (tw > 0 && th > 0 && (tx < 0 || tx >= tw || ty < 0 || ty >= th)) {
            const std::string msg = "tile (" + std::to_string(tx) + "," + std::to_string(ty) +
                                    ") out of map bounds " + std::to_string(tw) + "x" +
                                    std::to_string(th) + " (force to override)";
            return op_err(msg.c_str());
        }
        if (tx >= 0 && tx < NAV_W && ty >= 0 && ty < NAV_H) {
            const uint8_t* tiles = static_tiles_for(target);
            if (tiles != nullptr && (tiles[ty * NAV_W + tx] & TILE_BLOCK_BIT) != 0) {
                const std::string msg = "tile (" + std::to_string(tx) + "," + std::to_string(ty) +
                                        ") not walkable (force to override)";
                return op_err(msg.c_str());
            }
        }
    }
    std::string result;
    if (!transition_run_inline([target, tx, ty](std::string* out) {
            if (gamestate_state() != 0) {
                *out = op_err("map transition in progress");
                return;
            }
            void* ch = member_or_null(0);
            if (ch == nullptr) {
                *out = op_err("world not ready");
                return;
            }
            const int direction =
                *reinterpret_cast<uint8_t*>(reinterpret_cast<uint8_t*>(ch) + C_DIRECTION);
            fn_mapchange_set(target, tx, ty, direction);
            fn_gamestate_set_state(GAMESTATE_MAP_CHANGE);
            *out = op_ok();
        }, &result)) {
        return op_err(result.empty() ? "transition failed" : result.c_str());
    }
    return result;
}
std::string data_op_move(int32_t x, int32_t y) {
    if (!game_in_world()) return op_err("not in game");
    // v0.4.56：药水教学暂停自动取消（视为完成）后放行；早期 v0.4.41 在此拒绝
    if (const char* tb = tutorial_block_error()) {
        motion_task_stop();
        return op_err(tb);
    }
    // v0.5.43：UI 占据（对话框/面板/教学）时世界操作阻塞——游戏输入被 UI 接管
    if (const char* ui = ui_blocked()) {
        motion_task_stop();
        std::string err = "ui occupied: ";
        err += ui;
        return op_err(err.c_str());
    }
    void* ch = member_or_null(0);
    if (ch == nullptr) return op_err("role not found");
    if (fn_char_move == nullptr) return op_err("symbol not resolved");
    int px = *reinterpret_cast<int16_t*>(reinterpret_cast<uint8_t*>(ch) + C_POS_X);
    int py = *reinterpret_cast<int16_t*>(reinterpret_cast<uint8_t*>(ch) + C_POS_Y);
    // v0.4.29 自研 BFS 寻路（替代 CHAR_SearchPath：游戏寻路不能绕远路）
    // v0.4.45：全局规划无视动态单位（use_units=false）——只走静态地形，路径短；撞墙时局部短途绕行
    // v0.4.47：全局规划改回 use_units=true——use_units=false 路径穿过 type==2 单位（火把/装饰）
    //   导致 CHAR_Move 撞墙卡死（真机实测 tile(19,20) 目标 tile(24,20) 有 slot7 单位占据）。
    NavPath np;
    if (!nav_bfs(px >> 4, py >> 4, x >> 4, y >> 4, np, true) || np.dir_count == 0)
        return op_err("no path");
    // 注册导航帧任务：方向序列逐帧 CHAR_Move 驱动（NavCtx 静态实例，单任务语义）
    // 目标不可达（如 NPC 被视作阻挡）→ 走到最近可达点后转身面向目标（v0.4.29 改进）
    static NavCtx nav_ctx;
    nav_ctx.ch = ch;
    nav_ctx.dir_count = np.dir_count;
    nav_ctx.dir_idx = -1;
    nav_ctx.target_px = px;
    nav_ctx.target_py = py;
    nav_ctx.target_tx = x >> 4;
    nav_ctx.target_ty = y >> 4;
    nav_ctx.face_target = !np.found;
    nav_ctx.final_tx = x >> 4;
    nav_ctx.final_ty = y >> 4;
    nav_ctx.replan_count = 0;
    nav_ctx.wait_frames = 0;
    // v0.5.36：保存首次 BFS 原始路径，供 nav_task_tick 判断「回到原路径后续格」才清零重试计数
    nav_ctx.orig_dir_count = np.dir_count;
    nav_ctx.orig_sx = px >> 4;
    nav_ctx.orig_sy = py >> 4;
    nav_ctx.orig_progress = 0;
    for (int i = 0; i < np.dir_count; ++i) nav_ctx.orig_dirs[i] = static_cast<int8_t>(np.dirs[i]);
    for (int i = 0; i < np.dir_count; ++i) nav_ctx.dirs[i] = static_cast<int8_t>(np.dirs[i]);
    if (!motion_task_start(nav_task_tick, &nav_ctx)) return op_err("move start failed");
    return op_ok();
}
std::string data_op_walk(int32_t direction) {
    if (!game_in_world()) return op_err("not in game");
    // v0.4.56：药水教学暂停自动取消（视为完成）后放行；早期 v0.4.41 在此拒绝
    if (const char* tb = tutorial_block_error()) {
        motion_task_stop();
        return op_err(tb);
    }
    // v0.5.43：UI 占据阻塞
    if (const char* ui = ui_blocked()) {
        motion_task_stop();
        std::string err = "ui occupied: ";
        err += ui;
        return op_err(err.c_str());
    }
    if (direction < 0 || direction > 3) return op_err("bad direction");
    void* ch = member_or_null(0);
    if (ch == nullptr) return op_err("role not found");
    if (fn_char_move == nullptr) return op_err("symbol not resolved");
    // 注册帧任务：每帧（59ms）CHAR_Move(flag=0) 走 1 步累计 60 帧（v0.4.26）
    // 单任务语义：WalkCtx 用静态实例（同时只有一个任务），注册即重置
    static WalkCtx walk_ctx;
    walk_ctx.ch = ch;
    walk_ctx.dir = direction;
    walk_ctx.remaining = 60;
    walk_ctx.first_tick = true;   // v0.4.57：首帧缓冲
    if (!motion_task_start(walk_task_tick, &walk_ctx)) return op_err("walk start failed");
    return op_ok();
}
std::string data_op_walk_stop() {
    if (!game_in_world()) return op_err("not in game");
    if (const char* ui = ui_blocked()) {
        std::string err = "ui occupied: ";
        err += ui;
        return op_err(err.c_str());
    }
    void* ch = member_or_null(0);
    if (ch == nullptr) return op_err("role not found");
    motion_task_stop();
    if (fn_char_remove_path == nullptr) return op_err("symbol not resolved");
    fn_char_remove_path(ch);
    return op_ok();
}
std::string data_op_interact() {
    if (!game_in_world()) return op_err("not in game");
    if (const char* ui = ui_blocked()) {
        std::string err = "ui occupied: ";
        err += ui;
        return op_err(err.c_str());
    }
    if (fn_evtsystem_do_check_all_event == nullptr) return op_err("symbol not resolved");
    fn_evtsystem_do_check_all_event(2);
    return op_ok();
}


