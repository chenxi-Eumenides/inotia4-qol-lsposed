#include "plugins/quest/feature.h"

#include <atomic>

#include "core/native/qol_log.h"
#include "game/access/quest.h"
#include "game_state.h"
#include "infra/scheduler.h"

namespace qol::features::quest {
namespace {

std::atomic<bool> g_gate{false};

}  // namespace

void set_gate(bool enabled) {
    g_gate.store(enabled, std::memory_order_release);
}

bool gate_enabled() {
    return g_gate.load(std::memory_order_acquire);
}

void on_save_enter(void* ctx) {
    (void)ctx;
    if (!g_gate.load(std::memory_order_acquire)) return;  // deactivate 后 no-op
    // world 已就绪；在下一个逻辑帧点执行一次（count=1，触发后自动注销）。
    infra::scheduler_add(infra::FramePoint::kLogicPre, &resync_tick, nullptr, 1, 1);
}

bool resync_tick(int64_t frame, void* ctx) {
    (void)frame;
    (void)ctx;
    if (!g_gate.load(std::memory_order_acquire)) return false;  // 一次性：无论如何注销
    if (!game_in_world()) return false;

    uint32_t fixed = 0;
    const uint32_t count = game::quest_active_slot_count();
    for (uint32_t i = 0; i < count; ++i) {
        uint16_t quest_id = 0;
        if (!game::quest_active_slot_id(i, &quest_id)) continue;
        if (game::quest_state(quest_id) != 1) continue;   // 只补「进行中」
        if (!game::quest_is_complete(quest_id)) continue;  // 游戏自判达成
        if (game::quest_set_state(quest_id, 2)) ++fixed;   // 1 -> 2 补升
    }
    QOL_LOG_INFO(QolDomain::kPlatform, "quest resync: fixed=%u", fixed);
    return false;  // 一次性：返回 false 自动注销
}

}  // namespace qol::features::quest
