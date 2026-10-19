#include "game/access/world.h"

#include "game/access/game_access.h"

namespace qol::game {

bool world_ready() {
    return g_base != 0;
}

int world_state() {
    if (g_state == nullptr) return -1;
    return static_cast<int>(*reinterpret_cast<uint16_t*>(g_state));
}

int world_game_state_gate() {
    if (g_gamestate == nullptr) return 0;
    return static_cast<int>(*reinterpret_cast<uint32_t*>(g_gamestate));
}

int world_map_record_count() {
    if (g_mapinfo_record_count == nullptr ||
        !game_memory_accessible(g_mapinfo_record_count, sizeof(uint16_t), 'r')) {
        return 0;
    }
    return static_cast<int>(*reinterpret_cast<uint16_t*>(g_mapinfo_record_count));
}

const char* world_map_name(int map_id) {
    if (g_mapinfo_pdata_got == nullptr || g_mapinfo_record_size == nullptr ||
        g_mapinfo_record_count == nullptr || fn_memorytext_get_text == nullptr) {
        return nullptr;
    }
    auto* pdata_slot = reinterpret_cast<void**>(g_mapinfo_pdata_got);
    if (!game_memory_accessible(pdata_slot, sizeof(void*), 'r') || *pdata_slot == nullptr) {
        return nullptr;
    }
    auto* records = *reinterpret_cast<uint8_t**>(*pdata_slot);
    const uint8_t record_size = *reinterpret_cast<uint8_t*>(g_mapinfo_record_size);
    const uint16_t record_count = *reinterpret_cast<uint16_t*>(g_mapinfo_record_count);
    if (records == nullptr || record_size == 0 || map_id < 0 || map_id >= record_count) {
        return nullptr;
    }
    auto* record = records + static_cast<size_t>(map_id) * record_size;
    if (!game_memory_accessible(record, record_size, 'r')) return nullptr;
    const uint16_t text_id = *reinterpret_cast<uint16_t*>(record + MAPINFOBASE_RECORD_NAME_TEXT_ID);
    const char* text = fn_memorytext_get_text(text_id);
    if (text == nullptr || !game_memory_accessible(text, 1, 'r') || text[0] == '\0') {
        return nullptr;
    }
    return text;
}

int world_current_map_id() {
    return static_cast<int>(current_map_id());
}

bool world_map_change_ready() {
    return fn_mapchange_set != nullptr && fn_gamestate_set_state != nullptr;
}

void world_request_map_change(int map_id, int x, int y, int dir) {
    if (!world_map_change_ready()) return;
    fn_mapchange_set(map_id, x, y, dir);
    fn_gamestate_set_state(static_cast<int32_t>(GAMESTATE_MAP_CHANGE));
}

int world_active_player_direction() {
    void* player = nullptr;
    if (g_player_active_got != nullptr &&
        game_memory_accessible(g_player_active_got, sizeof(void*), 'r')) {
        void* player_global = *reinterpret_cast<void**>(g_player_active_got);
        if (player_global != nullptr &&
            game_memory_accessible(player_global, sizeof(void*), 'r')) {
            player = *reinterpret_cast<void**>(player_global);
        }
    }
    // 某些版本的 GOT 槽直接保存角色对象；已有 PLAYER_pActivePlayer 全局作为安全回退。
    if (player == nullptr && g_player_active != nullptr &&
        game_memory_accessible(g_player_active, sizeof(void*), 'r')) {
        player = *reinterpret_cast<void**>(g_player_active);
    }
    if (player == nullptr) return -1;
    auto* bytes = reinterpret_cast<uint8_t*>(player);
    if (!game_memory_accessible(bytes + C_DIRECTION, sizeof(uint8_t), 'r')) return -1;
    return *reinterpret_cast<uint8_t*>(bytes + C_DIRECTION);
}

bool world_money_ready() {
    return fn_get_money != nullptr && fn_minus_money != nullptr;
}

int64_t world_money() {
    return fn_get_money != nullptr ? fn_get_money() : 0;
}

bool world_minus_money(int64_t amount) {
    return fn_minus_money != nullptr && fn_minus_money(amount) != 0;
}

bool world_create_yesno_ready() {
    return fn_popup_create_yesno != nullptr;
}

bool world_create_yesno(const char* text, uint32_t len, void* ok, void* cancel, void* param) {
    if (fn_popup_create_yesno == nullptr || text == nullptr) return false;
    fn_popup_create_yesno(const_cast<char*>(text), len, 0, 2, ok, cancel, param);
    return true;
}

bool world_close_popup_ready() {
    return fn_ui_set_popup_process_info != nullptr;
}

bool world_close_popup_process() {
    if (fn_ui_set_popup_process_info == nullptr) return false;
    fn_ui_set_popup_process_info(3, 0);
    return true;
}

void world_restore_hud_gate() {
    if (g_base == 0) return;
    auto** hud_gate = reinterpret_cast<uint8_t**>(g_base + G_HUD_GATE_GOT_VMA);
    if (hud_gate != nullptr && *hud_gate != nullptr) **hud_gate = 1;
}

bool world_show_banner(const char* text) {
    if (fn_instantmsg_add == nullptr || text == nullptr) return false;
    fn_instantmsg_add(3, const_cast<char*>(text), 0, 0, 5, 0x15, 0, 0);
    return true;
}

int world_choice_state_id() {
    if (g_base == 0) return -1;
    auto* list_slot = reinterpret_cast<void**>(g_base + G_POPUP_STATE_LIST_GOT_VMA);
    if (!game_memory_accessible(list_slot, sizeof(void*), 'r') || *list_slot == nullptr) {
        return -1;
    }
    auto* list = reinterpret_cast<uint8_t*>(*list_slot);
    for (size_t index = 0; index < POPUP_STATE_COUNT; ++index) {
        auto* entry = list + index * POPUP_ENTRY_SIZE;
        const uintptr_t enter = *reinterpret_cast<uintptr_t*>(entry + POPUP_ENTRY_ENTER);
        if (enter == g_base + F_PANEL_CHOICE_ENTER) {
            return static_cast<int>(*reinterpret_cast<uint32_t*>(entry));
        }
    }
    return -1;
}

}  // namespace qol::game

// ---------------------------------------------------------------------------
// T2l-w-write：世界写原语（迁移自 api/native 世界写实现 teleport/direct/move/walk/
// walk_stop/interact）。infra 帧任务/转场经 contracts/runtime_hooks.h 过渡注入面取用
// （game 层零 infra include）；帧任务体沿用原日志调用点文本，经 qol_log_seam.h 重定义宏承接。
// ---------------------------------------------------------------------------
#include "contracts/runtime_hooks.h"
#include "game/access/qol_log_seam.h"
#include "game/nav/game_nav.h"
#include "game/ops/game_ops_common.h"
#include "game/read/game_tiles.h"
#include "game/state/game_state.h"

#include <cstring>
#include <string>

#include <cstdint>

namespace qol::game {
namespace {

constexpr int32_t kWorldTransitionTimeoutMs = 5000;

// 本地宏：调用点文本零改动（先例 5890b5a4 同法）；dom 参数被忽略。
#define QOL_LOG_DEBUG_EVERY(dom, every_frames, ...)                    \
    do {                                                               \
        static int64_t _seam_last_frame = INT64_MIN;                   \
        if (qol::game::qol_seam_debug_enabled() &&                     \
            qol::game::qol_seam_due(&_seam_last_frame, (every_frames))) \
            qol::game::qol_seam_write(qol::game::kQolLogLevelDebug, qol::game::kQolLogDomainApi,\
                                 __FILE__, __LINE__, __VA_ARGS__); \
    } while (0)

uint32_t gamestate_state() {
    return g_gamestate != nullptr ? *reinterpret_cast<uint32_t*>(g_gamestate) : 0;
}

// 移动任务互斥槽：nav/walk 共用，注册即替换并取消上一个；仅此槽可被 stop 清空。
// 不塞进通用注册表，避免 stop 误注销自动出售等其它帧任务。
uint64_t g_motion_task = 0;

bool walk_task_tick(int64_t /*frame*/, void* ctx);
bool nav_task_tick(int64_t /*frame*/, void* ctx);

// POD 帧回调适配（contracts 表返回 int32_t；tick 体逐字保留 bool）。
int32_t walk_tick_adapter(int64_t frame, void* ctx) { return walk_task_tick(frame, ctx) ? 1 : 0; }
int32_t nav_tick_adapter(int64_t frame, void* ctx) { return nav_task_tick(frame, ctx) ? 1 : 0; }

bool motion_task_start(bool (*fn)(int64_t, void*), void* ctx) {
    if (!game_in_world()) return false;
    if (g_gamestate != nullptr && *reinterpret_cast<uint32_t*>(g_gamestate) != 0) return false;
    if (g_motion_task != 0) qol::contracts::qol_runtime_hooks().frame_task_remove(g_motion_task);
    const contracts::QolRuntimeFrameTaskFn adapter =
        fn == &nav_task_tick ? &nav_tick_adapter : &walk_tick_adapter;
    g_motion_task = qol::contracts::qol_runtime_hooks().frame_task_add(
        contracts::kQolRuntimeFramePointRenderPre, adapter, ctx, 0, 0);
    return g_motion_task != 0;
}

void motion_task_stop() {
    if (g_motion_task != 0) {
        qol::contracts::qol_runtime_hooks().frame_task_remove(g_motion_task);
        g_motion_task = 0;
    }
}

// 复刻 CHAR_PickItemAll 数据路径：遍历掉落物数组 + 范围判断 + NOTIFIER_Add 排入主线程回调，
// 跳过其末尾的 SOUNDSYSTEM_Play 拾取音效（后台线程音频引擎句柄为空 → fault 0x0 崩溃）。
// 主线程 NOTIFIER_Process → CHAR_ActivePickupEvent(0xdd15c) 完成 INVEN_SaveItem 入库 + 移除掉落。
void nav_pick_items(void* ch, int radius) {
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
int on_orig_path(const NavCtx* n, int tx, int ty) {
    int cx = n->orig_sx, cy = n->orig_sy;
    if (cx == tx && cy == ty) return 0;
    for (int i = 0; i < n->orig_dir_count; ++i) {
        cx += NAV_DX[n->orig_dirs[i]];
        cy += NAV_DY[n->orig_dirs[i]];
        if (cx == tx && cy == ty) return i + 1;
    }
    return -1;
}

bool walk_task_tick(int64_t /*frame*/, void* ctx) {
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

bool nav_task_tick(int64_t /*frame*/, void* ctx) {
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
            QOL_LOG_DEBUG_EVERY(QolDomain::kApi, 60,
                                "replan: wait-retry ok dirs=%d", retry.dir_count);
            return !map_link_check(n->ch);
        }
        ++n->wait_frames;
        if (n->wait_frames > 60) {
            n->wait_frames = 0;
            QOL_LOG_DEBUG_EVERY(QolDomain::kApi, 60,
                                "replan: wait timeout at (%d,%d)", px >> 4, py >> 4);
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
                QOL_LOG_DEBUG_EVERY(QolDomain::kApi, 60,
                                    "replan: reached nearest (%d,%d), waiting", px >> 4, py >> 4);
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
            QOL_LOG_DEBUG_EVERY(QolDomain::kApi, 60,
                                "replan: abort target=(%d,%d) replan=%d",
                                n->target_tx, n->target_ty, n->replan_count);
            return false;
        }
        int cpx = px >> 4, cpy = py >> 4;
        // 撞墙格：撞墙方向的目标格——模块判可走但引擎 CHAR_Move 判阻挡（碰撞建模差异），
        // 重规划 BFS 临时排除它，避免规划又走同格反复撞墙（v0.5.27 变体根因缓解）。
        int wall_tx = -1, wall_ty = -1;
        int wall_dir = (n->dir_idx >= 0 && n->dir_idx < n->dir_count) ? n->dirs[n->dir_idx] : -1;
        if (wall_dir >= 0) { wall_tx = cpx + NAV_DX[wall_dir]; wall_ty = cpy + NAV_DY[wall_dir]; }
        QOL_LOG_DEBUG_EVERY(QolDomain::kApi, 60,
                            "replan: hit wall at (%d,%d) dir=%d replan=%d wall=(%d,%d)", cpx, cpy,
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
            QOL_LOG_DEBUG_EVERY(QolDomain::kApi, 60,
                                "replan: cur-path ok dirs=%d found=%d nearest=(%d,%d)",
                                np.dir_count, np.found ? 1 : 0, np.nearest_x, np.nearest_y);
            return !map_link_check(n->ch);
        }
        // 全量规划失败：终止任务（v0.4.51：不再多余尝试）
        QOL_LOG_DEBUG_EVERY(QolDomain::kApi, 60,
                            "replan: fail, terminate at (%d,%d)", cpx, cpy);
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

#undef QOL_LOG_DEBUG_EVERY

// op 信封字节由 op_ok()/op_err() 生成（game/ops，含成功力刷恰一次语义），拷入定长 POD。
WorldWriteResult pack_result(const std::string& body) {
    WorldWriteResult out{};
    const size_t size = body.size() < sizeof(out.text) ? body.size() : sizeof(out.text);
    if (size > 0) std::memcpy(out.text, body.data(), size);
    out.size = static_cast<uint32_t>(size);
    return out;
}

// 转场结果渲染：executed=true 时 out 为 body 写的完整 op JSON；false 时 out 为 busy/超时纯文本
// （与迁移前调用点 `op_err(result.empty() ? <fallback> : result)` 逐字一致）。
std::string render_transition(const QolResult& result, const char* body, uint32_t size,
                              const char* fallback) {
    const std::string text(body, size);
    if (!contracts::succeeded(result)) return op_err(text.empty() ? fallback : text.c_str());
    return text;
}

struct TeleportCtx {
    int32_t target;
    int32_t tx;
    int32_t ty;
};

void teleport_body(void* ctx, char* out, uint32_t capacity, uint32_t* out_len) {
    const auto* c = static_cast<const TeleportCtx*>(ctx);
    std::string body;
    if (gamestate_state() != 0) {
        body = op_err("map transition in progress");
    } else {
        void* ch = member_or_null(0);
        if (ch == nullptr) {
            body = op_err("world not ready");
        } else {
            const int direction =
                *reinterpret_cast<uint8_t*>(reinterpret_cast<uint8_t*>(ch) + C_DIRECTION);
            fn_mapchange_set(c->target, c->tx, c->ty, direction);
            fn_gamestate_set_state(GAMESTATE_MAP_CHANGE);
            body = op_ok();
        }
    }
    const size_t size = body.size() < capacity ? body.size() : capacity;
    if (size > 0) std::memcpy(out, body.data(), size);
    *out_len = static_cast<uint32_t>(size);
}

// 非 force 时的地界/阻挡校验；返回空串=通过，否则错误文案。
std::string teleport_validate(int32_t target, int32_t tx, int32_t ty, bool force) {
    if (force) return std::string();
    const int tw = static_tiles_width(target);
    const int th = static_tiles_height(target);
    if (tw > 0 && th > 0 && (tx < 0 || tx >= tw || ty < 0 || ty >= th)) {
        return "tile (" + std::to_string(tx) + "," + std::to_string(ty) + ") out of map bounds " +
               std::to_string(tw) + "x" + std::to_string(th) + " (force to override)";
    }
    if (tx >= 0 && tx < NAV_W && ty >= 0 && ty < NAV_H) {
        const uint8_t* tiles = static_tiles_for(target);
        if (tiles != nullptr && (tiles[ty * NAV_W + tx] & TILE_BLOCK_BIT) != 0) {
            return "tile (" + std::to_string(tx) + "," + std::to_string(ty) +
                   ") not walkable (force to override)";
        }
    }
    return std::string();
}

WorldWriteResult teleport_impl(int32_t map_id, int32_t x, int32_t y, bool force, bool blocking) {
    if (!game_in_world()) return pack_result(op_err("not in game"));
    if (const char* ui = ui_blocked()) {
        std::string err = "ui occupied: ";
        err += ui;
        return pack_result(op_err(err.c_str()));
    }
    if (member_or_null(0) == nullptr) return pack_result(op_err("world not ready"));
    if (fn_mapchange_set == nullptr || fn_gamestate_set_state == nullptr) {
        return pack_result(op_err("symbol not resolved"));
    }
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
    const std::string invalid = teleport_validate(target, tx, ty, force);
    if (!invalid.empty()) return pack_result(op_err(invalid.c_str()));
    // 原版切图只发生在世界态稳定后，且发起点全部在游戏主线程（走出口 tile 的
    // GoMapLinkByChar / 世界传送）。API 曾在 HTTP worker 线程直调 fn_change_map，与
    // 渲染并发导致 SPR_Draw SIGSEGV（真机 tombstone 两次复现）——与 main_menu/enter_slot
    // 同机理，切图发起必须经 transition_run 排到游戏主线程逻辑帧点执行。
    TeleportCtx ctx{target, tx, ty};
    char body[512]{};
    uint32_t size = 0;
    if (blocking) {
        const QolResult result = contracts::qol_runtime_hooks().transition_run(
            &teleport_body, &ctx, kWorldTransitionTimeoutMs, body, sizeof(body), &size);
        return pack_result(render_transition(result, body, size, "transition timeout"));
    }
    const QolResult result = contracts::qol_runtime_hooks().transition_run_inline(
        &teleport_body, &ctx, body, sizeof(body), &size);
    return pack_result(render_transition(result, body, size, "transition failed"));
}

}  // namespace

WorldWriteResult world_teleport(int32_t map_id, int32_t x, int32_t y, bool force) {
    return teleport_impl(map_id, x, y, force, true);
}

WorldWriteResult world_teleport_direct(int32_t map_id, int32_t x, int32_t y, bool force) {
    return teleport_impl(map_id, x, y, force, false);
}

WorldWriteResult world_move(int32_t x, int32_t y) {
    std::string body;
    if (!game_in_world()) return pack_result(op_err("not in game"));
    // v0.4.56：药水教学暂停自动取消（视为完成）后放行；早期 v0.4.41 在此拒绝
    if (const char* tb = tutorial_block_error()) {
        motion_task_stop();
        return pack_result(op_err(tb));
    }
    // v0.5.43：UI 占据（对话框/面板/教学）时世界操作阻塞——游戏输入被 UI 接管
    if (const char* ui = ui_blocked()) {
        motion_task_stop();
        std::string err = "ui occupied: ";
        err += ui;
        return pack_result(op_err(err.c_str()));
    }
    void* ch = member_or_null(0);
    if (ch == nullptr) return pack_result(op_err("role not found"));
    if (fn_char_move == nullptr) return pack_result(op_err("symbol not resolved"));
    int px = *reinterpret_cast<int16_t*>(reinterpret_cast<uint8_t*>(ch) + C_POS_X);
    int py = *reinterpret_cast<int16_t*>(reinterpret_cast<uint8_t*>(ch) + C_POS_Y);
    // v0.4.29 自研 BFS 寻路（替代 CHAR_SearchPath：游戏寻路不能绕远路）
    // v0.4.45：全局规划无视动态单位（use_units=false）——只走静态地形，路径短；撞墙时局部短途绕行
    // v0.4.47：全局规划改回 use_units=true——use_units=false 路径穿过 type==2 单位（火把/装饰）
    //   导致 CHAR_Move 撞墙卡死（真机实测 tile(19,20) 目标 tile(24,20) 有 slot7 单位占据）。
    NavPath np;
    if (!nav_bfs(px >> 4, py >> 4, x >> 4, y >> 4, np, true) || np.dir_count == 0)
        return pack_result(op_err("no path"));
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
    if (!motion_task_start(nav_task_tick, &nav_ctx)) return pack_result(op_err("move start failed"));
    return pack_result(op_ok());
}

WorldWriteResult world_walk(int32_t direction) {
    if (!game_in_world()) return pack_result(op_err("not in game"));
    // v0.4.56：药水教学暂停自动取消（视为完成）后放行；早期 v0.4.41 在此拒绝
    if (const char* tb = tutorial_block_error()) {
        motion_task_stop();
        return pack_result(op_err(tb));
    }
    // v0.5.43：UI 占据阻塞
    if (const char* ui = ui_blocked()) {
        motion_task_stop();
        std::string err = "ui occupied: ";
        err += ui;
        return pack_result(op_err(err.c_str()));
    }
    if (direction < 0 || direction > 3) return pack_result(op_err("bad direction"));
    void* ch = member_or_null(0);
    if (ch == nullptr) return pack_result(op_err("role not found"));
    if (fn_char_move == nullptr) return pack_result(op_err("symbol not resolved"));
    // 注册帧任务：每帧（59ms）CHAR_Move(flag=0) 走 1 步累计 60 帧（v0.4.26）
    // 单任务语义：WalkCtx 用静态实例（同时只有一个任务），注册即重置
    static WalkCtx walk_ctx;
    walk_ctx.ch = ch;
    walk_ctx.dir = direction;
    walk_ctx.remaining = 60;
    walk_ctx.first_tick = true;   // v0.4.57：首帧缓冲
    if (!motion_task_start(walk_task_tick, &walk_ctx)) return pack_result(op_err("walk start failed"));
    return pack_result(op_ok());
}

WorldWriteResult world_walk_stop() {
    if (!game_in_world()) return pack_result(op_err("not in game"));
    if (const char* ui = ui_blocked()) {
        std::string err = "ui occupied: ";
        err += ui;
        return pack_result(op_err(err.c_str()));
    }
    void* ch = member_or_null(0);
    if (ch == nullptr) return pack_result(op_err("role not found"));
    motion_task_stop();
    if (fn_char_remove_path == nullptr) return pack_result(op_err("symbol not resolved"));
    fn_char_remove_path(ch);
    return pack_result(op_ok());
}

WorldWriteResult world_interact() {
    if (!game_in_world()) return pack_result(op_err("not in game"));
    if (const char* ui = ui_blocked()) {
        std::string err = "ui occupied: ";
        err += ui;
        return pack_result(op_err(err.c_str()));
    }
    if (fn_evtsystem_do_check_all_event == nullptr) return pack_result(op_err("symbol not resolved"));
    fn_evtsystem_do_check_all_event(2);
    return pack_result(op_ok());
}

}  // namespace qol::game

// ---------------------------------------------------------------------------
// T2l-d：剧情读原语（迁自 api/native/game_world_story.inc，逐字）。供 game 层 UI/dialog
// 读面使用（game 层不得 include legacy/world）。
// ---------------------------------------------------------------------------
#include "game/json/game_json.h"

namespace qol::game {

bool story_active() {
    if (g_base == 0) return false;
    uint32_t gs = g_gamestate != nullptr ? *reinterpret_cast<uint32_t*>(g_gamestate) : 0;
    return gs == 1;
}

std::string story_json() {
    if (g_base == 0) return "{\"active\":false}";
    std::string out = "{\"active\":true";
    void* teller = *reinterpret_cast<void**>(g_base + G_EVT_PTELLER_VMA);
    if (teller != nullptr && fn_get_name != nullptr) {
        char* name = fn_get_name(teller);
        if (name != nullptr) out += ",\"speaker\":\"" + json_escape(name) + "\"";
    }
    uint8_t* pt = *reinterpret_cast<uint8_t**>(g_base + G_EVT_PTEXT_VMA);
    if (pt != nullptr) {
        std::string text;
        for (int i = 0; i < 2048 && pt[i] != 0; ++i) text += static_cast<char>(pt[i]);
        out += ",\"text\":\"" + json_escape(text.c_str()) + "\"";
    }
    uint32_t idx = *reinterpret_cast<uint32_t*>(g_base + G_EVT_INDEX_VMA);
    uint32_t cnt = *reinterpret_cast<uint32_t*>(g_base + G_EVT_DATA_COUNT_VMA);
    out += ",\"index\":" + std::to_string(idx) + ",\"count\":" + std::to_string(cnt);
    out += "}";
    return out;
}

}  // namespace qol::game
