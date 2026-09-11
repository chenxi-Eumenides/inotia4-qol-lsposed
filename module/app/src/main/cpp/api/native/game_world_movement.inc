static uint32_t gamestate_state() {
    return g_gamestate != nullptr ? *reinterpret_cast<uint32_t*>(g_gamestate) : 0;
}
// 复刻 CHAR_PickItemAll 数据路径：遍历掉落物数组 + 范围判断 + NOTIFIER_Add 排入主线程回调，
// 跳过其末尾的 SOUNDSYSTEM_Play 拾取音效（后台线程音频引擎句柄为空 → fault 0x0 崩溃）。
// 主线程 NOTIFIER_Process → CHAR_ActivePickupEvent(0xdd15c) 完成 INVEN_SaveItem 入库 + 移除掉落。
static void nav_pick_items(void* ch, int radius) {
    if (g_base == 0 || ch == nullptr || fn_mem_malloc == nullptr || fn_notifier_add == nullptr) return;
    int8_t* cnt = *reinterpret_cast<int8_t**>(g_base + G_DROP_COUNT_GOT_VMA);
    if (cnt == nullptr || *cnt <= 0) return;
    uint8_t* arr = *reinterpret_cast<uint8_t**>(*reinterpret_cast<void**>(g_base + G_DROP_ARRAY_GOT_VMA));
    if (arr == nullptr) return;
    void* cb = *reinterpret_cast<void**>(g_base + G_NOTIFIER_PICKUP_SLOT_VMA);
    if (cb == nullptr) return;
    int16_t px = *reinterpret_cast<int16_t*>(reinterpret_cast<uint8_t*>(ch) + C_POS_X);
    int16_t py = *reinterpret_cast<int16_t*>(reinterpret_cast<uint8_t*>(ch) + C_POS_Y);
    int seq = 0;
    for (int i = 0; i < static_cast<int>(*cnt); ++i) {
        uint8_t* e = arr + i * GROUND_ITEM_SIZE;
        int16_t ex = *reinterpret_cast<int16_t*>(e + GROUND_ITEM_X);
        int16_t ey = *reinterpret_cast<int16_t*>(e + GROUND_ITEM_Y);
        if (ex < px - radius || ex > px + radius) continue;
        if (ey < py - radius || ey > py + radius) continue;
        if (e[GROUND_ITEM_FLAGS] & GROUND_ITEM_FLAG_PICKING) continue;  // bit1 拾取中
        void* node = fn_mem_malloc(PICKUP_EVENT_SIZE);
        if (node == nullptr) continue;
        uint8_t* nd = reinterpret_cast<uint8_t*>(node);
        *reinterpret_cast<void**>(nd + PICKUP_EVENT_CHAR) = ch;                              // 玩家 char
        *reinterpret_cast<int32_t*>(nd + PICKUP_EVENT_X) = px;                               // 玩家 x
        *reinterpret_cast<int32_t*>(nd + PICKUP_EVENT_Y) = py;                               // 玩家 y
        *reinterpret_cast<void**>(nd + PICKUP_EVENT_OBJECT) = *reinterpret_cast<void**>(e + GROUND_ITEM_OBJECT); // 掉落物对象
        e[GROUND_ITEM_FLAGS] |= GROUND_ITEM_FLAG_PICKING;                                    // 拾取中标志
        fn_notifier_add(1, seq, cb, node);
        seq += 2;
    }
}
bool map_link_check(void* ch) {
    if (fn_go_map_link_by_char == nullptr || ch == nullptr) return false;
    int16_t px = *reinterpret_cast<int16_t*>(reinterpret_cast<uint8_t*>(ch) + C_POS_X);
    int16_t py = *reinterpret_cast<int16_t*>(reinterpret_cast<uint8_t*>(ch) + C_POS_Y);
    // v0.5.28 修复：GoMapLinkByChar 实际 4 参 (ch, tx, ty, use_dir)，use_dir=0 走 MAP_FindMapLinkNoDir
    // （只查出口 tile 坐标，不查角色朝向）。此前 3 参调用致 use_dir 为垃圾值 → CheckMapLink 走
    // MAP_FindMapLink 按朝向匹配 → 朝向不符返回 null → 出口 tile 概率性不切图（真机 (36,26)/(37,26)）。
    return fn_go_map_link_by_char(ch, px >> 4, py >> 4, 0) != 0;
}
struct WalkCtx {
    void* ch;
    int dir;
    int remaining;
    bool first_tick;   // v0.4.57：首帧缓冲标志（帧驱动下注册瞬间即执行第一步）
};
struct NavCtx {
    void* ch = nullptr;
    int dir_count = 0;
    int dir_idx = 0;
    int target_px = 0, target_py = 0;
    int target_tx = -1, target_ty = -1;
    bool face_target = false;
    int final_tx = -1, final_ty = -1;
    int replan_count = 0;
    int wait_frames = 0;   // v0.5.27：走到最近可达点后等待动态单位移开的帧计数
    int8_t dirs[NAV_MAX_DIRS];
    // v0.5.36：原始路径（首次 BFS 结果）——撞墙绕行后仅回到「未走过的后续格」才清零 replan_count
    int8_t orig_dirs[NAV_MAX_DIRS];
    int orig_dir_count = 0;
    int orig_sx = -1, orig_sy = -1;  // 原始路径起点 tile
    int orig_progress = 0;           // 原路径上已推进到的最大下标（0=起点，已走过）
};

// 玩家 tile 在原始路径上的下标（0=起点）；不在原路径上返回 -1
static int on_orig_path(const NavCtx* n, int tx, int ty) {
    int cx = n->orig_sx, cy = n->orig_sy;
    if (cx == tx && cy == ty) return 0;
    for (int i = 0; i < n->orig_dir_count; ++i) {
        cx += NAV_DX[n->orig_dirs[i]];
        cy += NAV_DY[n->orig_dirs[i]];
        if (cx == tx && cy == ty) return i + 1;
    }
    return -1;
}

bool walk_task_tick(void* ctx) {
    WalkCtx* w = static_cast<WalkCtx*>(ctx);
    if (fn_char_move == nullptr || w == nullptr) return false;
    // v0.4.37 起在剧情/切图态禁止后台线程继续 CHAR_Move（与状态机竞争会破坏控制态）。
    // P0 修复：剧情态(EVENT=1)由 CHAR_Move 内部触发，需暂停等剧情播放完再切图；
    // 切图态(MAP_CHANGE=3)路径已失效，立即终止。
    uint32_t gs = gamestate_state();
    if (gs == GAMESTATE_MAP_CHANGE) return false;
    if (gs != 0) return true;
    // v0.5.43：UI 占据（对话框/面板打开）时暂停移动帧任务（与剧情态语义对齐）
    if (ui_blocked() != nullptr) return true;
    // v0.4.57 首帧缓冲：帧驱动下注册瞬间即执行第一步，此时角色可能处于
    // 上一操作收尾状态（CHAR_Move 状态未复位）——首帧仅设朝向，下一帧才走（与 nav_task_tick 对齐）
    if (w->first_tick) {
        w->first_tick = false;
        if (fn_char_set_direction != nullptr) fn_char_set_direction(w->ch, w->dir);
        return true;
    }
    // flag=0：CHAR_Move 内部自动 MAP_SetFocus 跟随摄像机。
    // 返回值：0=正常走一步（成功），非 0=撞墙/阻挡（反汇编 e98dc mov w20,#0x1）
    // v0.4.40：CHAR_Move 不更新朝向（官方链=按键→SetDirection+Move 分开调），移动前先设朝向避免"飘逸"
    if (fn_char_set_direction != nullptr) fn_char_set_direction(w->ch, w->dir);
    if (fn_char_move(w->ch, w->dir, 8, 0)) return false;  // 撞墙/不可走
    // CHAR_Move 内部剧情检查可能已触发 SetReady→SetState(1)（剧情态）：跳过切图检查，
    // 让剧情先播放（对齐官方"先剧情后切图"），剧情结束后下一帧恢复再继续。
    if (gamestate_state() != 0) return true;
    // 对齐官方移动按键链 GAMESTATE_PressKeyPlay 0x9d10c：移动后拾取脚下掉落物（半径 0x18=24px）。
    nav_pick_items(w->ch, 0x18);
    return --w->remaining > 0 && !map_link_check(w->ch);  // 走完 60 帧或切图终止
}
bool nav_task_tick(void* ctx) {
    NavCtx* n = static_cast<NavCtx*>(ctx);
    if (n == nullptr || n->ch == nullptr || fn_char_move == nullptr) return false;
    // v0.4.37 起在剧情/切图态禁止后台线程继续 CHAR_Move（与状态机竞争会破坏控制态）。
    // P0 修复：剧情态(EVENT=1)由 CHAR_Move 内部触发，需暂停等剧情播放完再切图；
    // 切图态(MAP_CHANGE=3)路径已失效，立即终止。
    uint32_t gs = gamestate_state();
    if (gs == GAMESTATE_MAP_CHANGE) return false;
    if (gs != 0) return true;
    // v0.5.43：UI 占据（对话框/面板打开）时暂停导航帧任务（与剧情态语义对齐）
    if (ui_blocked() != nullptr) return true;
    uint8_t* ch = reinterpret_cast<uint8_t*>(n->ch);
    int px = *reinterpret_cast<int16_t*>(ch + C_POS_X);
    int py = *reinterpret_cast<int16_t*>(ch + C_POS_Y);
    // v0.5.27：等待重试态——走到最近可达点后等待动态单位移开，逐帧重试 BFS 接续
    if (n->wait_frames > 0) {
        NavPath retry;
        if (nav_bfs(px >> 4, py >> 4, n->target_tx, n->target_ty, retry, true) &&
            retry.dir_count > 0 && retry.found) {
            n->replan_count++;
            n->dir_count = retry.dir_count;
            n->dir_idx = -1;
            n->face_target = false;
            n->wait_frames = 0;
            n->target_px = px;
            n->target_py = py;
            for (int i = 0; i < retry.dir_count; ++i)
                n->dirs[i] = static_cast<int8_t>(retry.dirs[i]);
            MOVE_LOG("replan: wait-retry ok dirs=%d", retry.dir_count);
            return !map_link_check(n->ch);
        }
        ++n->wait_frames;
        if (n->wait_frames > 60) {
            n->wait_frames = 0;
            MOVE_LOG("replan: wait timeout at (%d,%d)", px >> 4, py >> 4);
            return false;
        }
        return true;   // 继续等待（保持帧任务存活）
    }
    if ((px - n->target_px < 8 && n->target_px - px < 8) &&
        (py - n->target_py < 8 && n->target_py - py < 8)) {
        n->dir_idx++;
        if (n->dir_idx >= n->dir_count) {
            if (n->face_target && n->final_tx >= 0) {
                // v0.5.27：走到最近可达点（目标被动态单位/静态阻挡）后，转身面向目标并
                // 进入等待重试态（wait_frames），等待动态单位（怪）移开后逐帧重试 BFS 接续，
                // 而非立即终止——修复"重规划卡死"（怪挡路时玩家停在原地不动、move_to 无法继续）。
                int dx = n->final_tx - (px >> 4), dy = n->final_ty - (py >> 4);
                int face_dir = (dx < 0) ? 1 : (dx > 0) ? 3 : (dy > 0) ? 0 : 2;
                if (fn_char_set_direction != nullptr) fn_char_set_direction(n->ch, face_dir);
                fn_char_move(n->ch, face_dir, 8, 0);
                n->face_target = false;
                n->wait_frames = 1;
                MOVE_LOG("replan: reached nearest (%d,%d), waiting", px >> 4, py >> 4);
                return true;
            }
            return false;
        }
        int d = n->dirs[n->dir_idx];
        n->target_px = (px >> 4) * 16 + 8 + NAV_DX[d] * 16;
        n->target_py = (py >> 4) * 16 + 8 + NAV_DY[d] * 16;
    }
    if (n->dir_idx < 0 || n->dir_idx >= n->dir_count) return false;
    // v0.4.40：CHAR_Move 不更新朝向，移动前先设朝向（nav 同样适用，避免路径移动"飘逸"）
    if (fn_char_set_direction != nullptr) fn_char_set_direction(n->ch, n->dirs[n->dir_idx]);
    if (fn_char_move(n->ch, n->dirs[n->dir_idx], 8, 0)) {
        // 撞墙/被动态单位阻挡 → 从当前格 BFS 绕行（排除撞墙格，见下 wall_tx/wall_ty）。
        if (n->target_tx < 0 || n->replan_count >= 5) {
            MOVE_LOG("replan: abort target=(%d,%d) replan=%d", n->target_tx, n->target_ty, n->replan_count);
            return false;
        }
        int cpx = px >> 4, cpy = py >> 4;
        // 撞墙格：撞墙方向的目标格——模块判可走但引擎 CHAR_Move 判阻挡（碰撞建模差异），
        // 重规划 BFS 临时排除它，避免规划又走同格反复撞墙（v0.5.27 变体根因缓解）。
        int wall_tx = -1, wall_ty = -1;
        int wall_dir = (n->dir_idx >= 0 && n->dir_idx < n->dir_count) ? n->dirs[n->dir_idx] : -1;
        if (wall_dir >= 0) { wall_tx = cpx + NAV_DX[wall_dir]; wall_ty = cpy + NAV_DY[wall_dir]; }
        MOVE_LOG("replan: hit wall at (%d,%d) dir=%d replan=%d wall=(%d,%d)", cpx, cpy,
                 wall_dir, n->replan_count, wall_tx, wall_ty);
        // v0.5.27：撞墙后从当前格 BFS 绕行（排除撞墙格）——废除 v0.4.53 的 resume 探测：
        // resume 格在障碍后方（玩家到不了），其路径起点与玩家位置错位，found=true 也会反复撞墙。
        // found=false（目标被动态单位封死）时不再直接终止——改用 nearest 最近可达点接续
        // （走到障碍物面前转身面向目标并等待重试），而非停在撞墙格不动（修复"重规划卡死"）。
        NavPath np;
        if (nav_bfs(cpx, cpy, n->target_tx, n->target_ty, np, true, 0, wall_tx, wall_ty) &&
            np.dir_count > 0) {
            n->replan_count++;
            n->dir_count = np.dir_count;
            n->dir_idx = -1;
            n->target_px = px;
            n->target_py = py;
            n->face_target = !np.found;
            for (int i = 0; i < np.dir_count; ++i) n->dirs[i] = static_cast<int8_t>(np.dirs[i]);
            MOVE_LOG("replan: cur-path ok dirs=%d found=%d nearest=(%d,%d)",
                     np.dir_count, np.found ? 1 : 0, np.nearest_x, np.nearest_y);
            return !map_link_check(n->ch);
        }
        // 全量规划失败：终止任务（v0.4.51：不再多余尝试）
        MOVE_LOG("replan: fail, terminate at (%d,%d)", cpx, cpy);
        return false;
    }
    // v0.5.36：成功走一步后仅当回到原始路径「未走过的后续格」才清零重试计数——
    // 绕行未回归原路径（或回到已走过的格子）时计数持续累积，防 A→B→A 撞墙振荡无限重规划
    // 注意用移动后坐标（px/py 为本帧移动前读取）
    int npx = *reinterpret_cast<int16_t*>(ch + C_POS_X);
    int npy = *reinterpret_cast<int16_t*>(ch + C_POS_Y);
    int oi = on_orig_path(n, npx >> 4, npy >> 4);
    if (oi > n->orig_progress) {
        n->orig_progress = oi;
        n->replan_count = 0;
    }
    // CHAR_Move 内部剧情检查可能已触发 SetReady→SetState(1)（剧情态）：跳过切图检查，
    // 让剧情先播放（对齐官方"先剧情后切图"），剧情结束后下一帧恢复再继续。
    if (gamestate_state() != 0) return true;
    // 对齐官方移动按键链 GAMESTATE_PressKeyPlay 0x9d10c：移动后拾取脚下掉落物（半径 0x18=24px）。
    nav_pick_items(n->ch, 0x18);
    return !map_link_check(n->ch);
}

