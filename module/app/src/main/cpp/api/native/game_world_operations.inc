std::string data_op_teleport(int32_t map_id, int32_t x, int32_t y) {
    if (!game_in_world()) return op_err("not in game");
    if (const char* ui = ui_blocked()) {
        std::string err = "ui occupied: ";
        err += ui;
        return op_err(err.c_str());
    }
    if (fn_change_map == nullptr || fn_set_position == nullptr)
        return op_err("symbol not resolved");
    if (map_id > 0) {
        fn_change_map(map_id, x, y, 0);
    } else if (fn_set_position != nullptr) {
        fn_set_position(x, y);
    }
    return op_ok();
}
std::string data_op_move(int32_t x, int32_t y) {
    if (!game_in_world()) return op_err("not in game");
    // v0.4.56：药水教学暂停自动取消（视为完成）后放行；早期 v0.4.41 在此拒绝
    if (const char* tb = tutorial_block_error()) {
        stop_all_tasks();
        return op_err(tb);
    }
    // v0.5.43：UI 占据（对话框/面板/教学）时世界操作阻塞——游戏输入被 UI 接管
    if (const char* ui = ui_blocked()) {
        stop_all_tasks();
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
    if (frame_task_register(nav_task_tick, &nav_ctx) == 0) return op_err("move start failed");
    return op_ok();
}
std::string data_op_walk(int32_t direction) {
    if (!game_in_world()) return op_err("not in game");
    // v0.4.56：药水教学暂停自动取消（视为完成）后放行；早期 v0.4.41 在此拒绝
    if (const char* tb = tutorial_block_error()) {
        stop_all_tasks();
        return op_err(tb);
    }
    // v0.5.43：UI 占据阻塞
    if (const char* ui = ui_blocked()) {
        stop_all_tasks();
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
    if (frame_task_register(walk_task_tick, &walk_ctx) == 0) return op_err("walk start failed");
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
    stop_all_tasks();
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


