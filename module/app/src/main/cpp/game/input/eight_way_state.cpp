#include "game/input/eight_way_state.h"

#include <atomic>
#include <cstdint>

#include "game/access/game_access.h"
#include "game/access/qol_log_seam.h"
#include "game/access/world.h"
#include "game/input/eight_way_access.h"
#include "game/input/eight_way_draw.h"
#include "game/input/eight_way_move_math.h"
#include "game/state/game_state.h"

namespace qol::game::input {
namespace {

std::atomic<bool> g_enabled{false};
State g_state;
void* g_last_lead = nullptr;

// 0x15 可见摇杆的半径（UIPlay_Create：252x252）；中心 = 控件绝对左上角 + 126。
constexpr int64_t kPadHalfSize = 126;
constexpr int64_t kFallbackPadCenterX = 10 + kPadHalfSize;
constexpr int64_t kFallbackPadCenterBottom = kPadHalfSize;
constexpr int32_t kTileSize = 16;
constexpr int32_t kTileHalf = 8;

int direction_key_index(int32_t key) {
    int32_t table[4]{};
    if (!access::read_key_table(table)) return -1;
    for (int i = 0; i < 4; ++i) {
        if (table[i] == key) return i;
    }
    return -1;
}

int32_t current_facing() {
    void* ch = lead_member();
    if (ch == nullptr) return -1;
    const auto* direction = reinterpret_cast<const int8_t*>(
        static_cast<const uint8_t*>(ch) + C_DIRECTION);
    if (!game_memory_accessible(direction, sizeof(int8_t), 'r')) return -1;
    const int32_t value = *direction;
    return value >= 0 && value <= 3 ? value : -1;
}

void log_first_touch(const State& state, int32_t dx, int32_t dy) {
    static std::atomic<bool> logged{false};
    if (logged.exchange(true, std::memory_order_acq_rel)) return;
    int32_t table[4]{};
    (void)access::read_key_table(table);
    QOL_LOG_SEAM_INFO(
        kQolLogDomainPlatform,
        "omni touch lattice=%d primary=%d secondary=%d steps=%d/%d dx=%d dy=%d keys=%d,%d,%d,%d",
        state.lattice_index, state.primary_dir, state.secondary_dir, state.primary_step,
        state.secondary_step, dx, dy, table[0], table[1], table[2], table[3]);
}

void clear_state() {
    g_state = State{};
}

// 死区/无效触点：清状态并显式写 0，抑制本帧移动。
// 原触摸回调已先写入普通方向键码，只清 state 不能阻止 CHAR_Move。
void suppress_motion() {
    const bool owns = g_state.owns_input;
    clear_state();
    access::write_key_code(0);
    access::write_press_active(false);
    if (owns) access::clear_wait_coord(lead_member());
}

// 把角色直接吸附到当前 16px 瓦片中心；不调用 CHAR_Move，因此不触发补步/格子事件。
// 瓦片中心一定 8px 对齐，且 16px 碰撞盒正好落在该瓦片内。
bool snap_to_tile_center(void* ch) {
    if (ch == nullptr) return false;
    int16_t x = 0;
    int16_t y = 0;
    if (!access::read_position(ch, &x, &y)) return false;
    if ((x & 7) == 0 && (y & 7) == 0) {
        access::clear_wait_coord(ch);
        return true;
    }

    const int32_t cx = (static_cast<int32_t>(x) / kTileSize) * kTileSize + kTileHalf;
    const int32_t cy = (static_cast<int32_t>(y) / kTileSize) * kTileSize + kTileHalf;
    if (access::tile_blocking(cx / kTileSize, cy / kTileSize) != 0) return false;
    if (!access::write_position(ch, static_cast<int16_t>(cx), static_cast<int16_t>(cy))) {
        return false;
    }
    access::clear_wait_coord(ch);
    return true;
}

}  // namespace

bool enabled() {
    return g_enabled.load(std::memory_order_acquire);
}

const State& current_state() {
    return g_state;
}

void reset_state() {
    const bool owns = g_state.owns_input;
    clear_state();
    if (!owns) return;
    // 只清本功能写入的输入：自定义码可能已被翻译成普通主方向码，必须连普通码和
    // 按住位一起清；否则回到 Play 或插件关闭后会继续移动。
    access::write_key_code(0);
    access::write_press_active(false);
    access::clear_wait_coord(lead_member());
}

void reset() {
    reset_state();
}

void set_enabled(uint8_t enabled_value) {
    const bool on = enabled_value != 0;
    const bool was_on = g_enabled.exchange(on, std::memory_order_acq_rel);
    if (on) {
        if (!was_on) {
            g_last_lead = lead_member();
            reset_state();
            prepare_direction_overlay();
        }
        return;
    }

    // 关闭前把主控恢复到 8px，避免把非对齐坐标留给原版 8px 逻辑。
    void* lead = lead_member();
    if (lead != nullptr && world_game_state_gate() == 0 && snap_to_tile_center(lead)) {
        int16_t x = 0;
        int16_t y = 0;
        if (access::read_position(lead, &x, &y)) {
            (void)access::set_camera_focus(x, y);
        }
    }
    reset_state();
    restore_direction_overlay();
    g_last_lead = nullptr;
}

void on_direction_touch(void* ctrl, uint64_t event, const void* point, uint64_t param3) {
    (void)param3;
    if (!enabled()) {
        reset_state();
        return;
    }

    const bool release = event == 2 || event == 4 || event == 8;
    if (release || !access::press_active()) {
        reset_state();
        return;
    }
    if (event != 1 && event != 0x40) return;
    if (point == nullptr) {
        suppress_motion();
        return;
    }

    const auto* xy = static_cast<const int64_t*>(point);
    int64_t offset_x = 0;
    int64_t offset_y = 0;
    (void)access::touch_offset(&offset_x, &offset_y);

    int64_t center_x = 0;
    int64_t center_y = 0;
    if (!access::control_absolute_point(ctrl, &center_x, &center_y)) {
        const int32_t height = access::screen_height();
        if (height <= 0) {
            suppress_motion();
            return;
        }
        center_x = kFallbackPadCenterX;
        center_y = height - kFallbackPadCenterBottom;
    } else {
        center_x += kPadHalfSize;
        center_y += kPadHalfSize;
    }

    const int64_t touch_x = offset_x + xy[0];
    const int64_t touch_y = offset_y + xy[1];
    const int32_t dx = static_cast<int32_t>(touch_x - center_x);
    const int32_t dy = static_cast<int32_t>(center_y - touch_y);

    const int32_t previous = g_state.active ? g_state.lattice_index : -1;
    const int32_t index = select_lattice_step(dx, dy, previous);
    if (index < 0) {
        suppress_motion();
        return;
    }

    int32_t primary_dir = -1;
    int32_t secondary_dir = -1;
    int32_t primary_step = 0;
    int32_t secondary_step = 0;
    int32_t primary_key_index = -1;
    if (!lattice_step_components(index, current_facing(), &primary_dir, &secondary_dir,
                                 &primary_step, &secondary_step, &primary_key_index)) {
        suppress_motion();
        return;
    }

    int32_t table[4]{};
    if (!access::read_key_table(table)) {
        suppress_motion();
        return;
    }
    const int32_t primary_key = table[primary_key_index];
    if (primary_key == 0) {
        suppress_motion();
        return;
    }

    g_state.active = true;
    g_state.trusted = false;
    g_state.lattice_index = index;
    g_state.primary_dir = primary_dir;
    g_state.secondary_dir = secondary_dir;
    g_state.primary_step = primary_step;
    g_state.secondary_step = secondary_step;
    g_state.primary_key = primary_key;
    g_state.raw_active = true;
    g_state.raw_dx = dx;
    g_state.raw_dy = dy;
    g_state.owns_input = true;

    access::write_key_code(custom_dir_key(0));
    log_first_touch(g_state, dx, dy);
}

int32_t translate_press_key(int32_t key) {
    if (!enabled()) return key;

    if (is_custom_dir_key(key)) {
        if (!g_state.active || g_state.primary_key == 0) {
            access::write_key_code(0);
            return 0;
        }
        // 必须在原函数返回前写回主方向键码：GAMESTATE_PressKey 之后会用它判断是否
        // 保持 KEY_bPressed（按住有效位）。
        access::write_key_code(g_state.primary_key);
        g_state.trusted = true;
        g_state.owns_input = true;
        // 预置朝向让原版转向闸门直接通过；非 8px 坐标由 CHAR_MoveOnWaitCoord hook 处理。
        access::prepare_touch_move(lead_member(), g_state.primary_dir);
        return g_state.primary_key;
    }

    const int index = direction_key_index(key);
    if (index < 0) {
        g_state.trusted = false;
        g_state.owns_input = false;
    } else if (!(g_state.active && g_state.trusted && key == g_state.primary_key &&
                 access::press_active())) {
        g_state.trusted = false;
        g_state.owns_input = false;
    } else {
        // 自定义码在上一帧已被翻译成普通主方向码；后续按住帧同样需要预置朝向。
        access::prepare_touch_move(lead_member(), g_state.primary_dir);
    }
    return key;
}

void watch_lead_change() {
    if (!enabled()) return;
    void* lead = lead_member();
    if (g_last_lead == nullptr) {
        g_last_lead = lead;
        return;
    }
    if (lead == g_last_lead) return;

    void* previous = g_last_lead;
    g_last_lead = lead;
    if (previous != nullptr) {
        const bool snapped = snap_to_tile_center(previous);
        static std::atomic<bool> logged{false};
        if (!snapped && !logged.exchange(true, std::memory_order_acq_rel)) {
            QOL_LOG_SEAM_WARN(kQolLogDomainPlatform,
                              "omni lead switch snap skipped (blocked or invalid)");
        }
    }
}

uint8_t handle_wait_coord(void* ch) {
    if (!enabled() || ch == nullptr) return 0;
    if (world_game_state_gate() != 0) return 0;
    if (ch != lead_member()) return 0;
    if (access::has_path(ch)) return 0;
    access::clear_wait_coord(ch);
    return 1;
}

void poll() {
    if (!enabled()) return;
    if (world_game_state_gate() != 0) {
        g_last_lead = lead_member();
        reset_state();
        return;
    }
    watch_lead_change();
}

}  // namespace qol::game::input
