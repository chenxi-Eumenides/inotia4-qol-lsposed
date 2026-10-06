#pragma once

#include <cstring>
#include <limits>

#include "game/access/game_access.h"

namespace qol::game {

struct NativeScrollState {
    int64_t offset;
    int64_t max_offset;
    int64_t key_step;
};

// 不把 ControlScroll data 布局或负位移传播到消费者。
inline bool native_scroll_state(void* scroll, NativeScrollState* out) {
    if (scroll == nullptr || out == nullptr || fn_ctrl_get_data == nullptr ||
        !game_memory_accessible(scroll, CO_SIZE, 'r')) return false;
    const auto* data = static_cast<const uint8_t*>(fn_ctrl_get_data(scroll));
    if (data == nullptr || !game_memory_accessible(data, CS_SIZE, 'r') ||
        data[CS_TYPE] != 1) return false;
    int64_t start, current, extent, view, step;
    std::memcpy(&start, data + CS_START, sizeof(start));
    std::memcpy(&current, data + CS_CURRENT, sizeof(current));
    std::memcpy(&extent, data + CS_CONTENT_EXTENT, sizeof(extent));
    std::memcpy(&view, data + CS_VIEW_EXTENT, sizeof(view));
    std::memcpy(&step, data + CS_KEY_STEP, sizeof(step));
    if (extent < 0 || view <= 0 || start < current) return false;
    const uint64_t offset = static_cast<uint64_t>(start) - static_cast<uint64_t>(current);
    if (offset > static_cast<uint64_t>(std::numeric_limits<int64_t>::max())) return false;
    *out = NativeScrollState{static_cast<int64_t>(offset), extent > view ? extent - view : 0, step};
    return true;
}

}  // namespace qol::game
