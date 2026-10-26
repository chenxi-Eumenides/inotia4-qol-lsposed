#include "game/input/eight_way_state.h"

#include <atomic>
#include <cstdint>

#include "game/access/game_access.h"
#include "game/access/qol_log_seam.h"
#include "game/access/world.h"
#include "game/input/eight_way_access.h"
#include "game/input/eight_way_move_math.h"
#include "game/state/game_state.h"

namespace qol::game::input {
namespace {

std::atomic<bool> g_enabled{false};
State g_state;

// 0x15 可见摇杆的半径（UIPlay_Create：252x252）；中心 = 控件绝对左上角 + 126。
constexpr int64_t kPadHalfSize = 126;
constexpr int64_t kFallbackPadCenterX = 10 + kPadHalfSize;
constexpr int64_t kFallbackPadCenterBottom = kPadHalfSize;

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

void log_first_touch(const EightWayClass& c, int32_t dx, int32_t dy) {
    static std::atomic<bool> logged{false};
    if (logged.exchange(true, std::memory_order_acq_rel)) return;
    int32_t table[4]{};
    (void)access::read_key_table(table);
    QOL_LOG_SEAM_INFO(
        kQolLogDomainPlatform,
        "eight_way touch sector=%d primary=%d secondary=%d dx=%d dy=%d keys=%d,%d,%d,%d",
        c.sector, c.primary_dir, c.secondary_dir, dx, dy, table[0], table[1], table[2],
        table[3]);
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
    // 按住位一起清；否则回到 Play 或插件关闭后会继续移动。物理键/其他按钮的输入
    // 在 translate_press_key 中已把 owns_input 置 false，不会被这里清掉。
    access::write_key_code(0);
    access::write_press_active(false);
    // 斜向步长可能让原版置过转向等待位；松手/关闭时一并清掉，
    // 否则 CHAR_Process 会在松手后继续 CHAR_MoveOnWaitCoord 补步。
    access::clear_wait_coord(lead_member());
}

void reset() {
    reset_state();
}

void set_enabled(uint8_t enabled_value) {
    const bool on = enabled_value != 0;
    g_enabled.store(on, std::memory_order_release);
    if (!on) reset_state();
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

    EightWayClass classified = classify_vector(dx, dy);
    const int32_t facing = current_facing();
    apply_facing_preference(classified, facing);
    if (classified.diagonal) {
        static std::atomic<bool> logged_diagonal{false};
        if (!logged_diagonal.exchange(true, std::memory_order_acq_rel)) {
            QOL_LOG_SEAM_INFO(kQolLogDomainPlatform,
                              "eight_way facing=%d primary=%d secondary=%d", facing,
                              classified.primary_dir, classified.secondary_dir);
        }
    }
    int32_t table[4]{};
    if (!classified.valid || !access::read_key_table(table)) {
        suppress_motion();
        return;
    }

    const int32_t primary_key = table[classified.primary_key_index];
    if (primary_key == 0) {
        suppress_motion();
        return;
    }

    g_state.active = true;
    g_state.trusted = false;
    g_state.sector = classified.sector;
    g_state.primary_dir = classified.primary_dir;
    g_state.secondary_dir = classified.secondary_dir;
    g_state.primary_key = primary_key;
    g_state.owns_input = true;

    access::write_key_code(custom_dir_key(classified.sector));
    log_first_touch(classified, dx, dy);
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
        // 预置朝向让原版转向闸门直接通过；即使角色当前不在 8px 对齐点，
        // 也不会进入 CHAR_MoveOnWaitCoord 补步。
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
        // 自定义码在上一帧已被翻译成普通主方向码；后续按住帧同样需要绕过
        // 8px 转向闸门，否则每次方向切换仍会退化到补步速度。
        access::prepare_touch_move(lead_member(), g_state.primary_dir);
    }
    return key;
}

void poll() {
    if (!enabled()) return;
    if (world_game_state_gate() != 0) reset_state();
}

}  // namespace qol::game::input
