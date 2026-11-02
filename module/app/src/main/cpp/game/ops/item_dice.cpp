#include "game/ops/item_dice.h"

#include "contracts/game_cache.h"
#include "game/access/game_access.h"
#include "game/state/game_state.h"

namespace qol::game::ops {

QolResult reject_dice() {
    if (!game_in_world()) return contracts::error(kQolErrNotReady, "not in game", 11);
    uint8_t* flag = *reinterpret_cast<uint8_t**>(g_base + G_STATUSDICE_FLAG_GOT_VMA);
    if (flag == nullptr || (*flag & 1u) == 0) {
        return contracts::error(kQolErrNotFound, "no dice result pending", 22);
    }
    *flag &= ~1u;
    frame_cache_force_refresh();
    return contracts::ok();
}

QolResult accept_dice(contracts::QolItemDiceAcceptResult* out) {
    if (out == nullptr) return contracts::error(kQolErrInvalidArgument, "invalid dice result", 19);
    *out = {};
    if (!game_in_world()) return contracts::error(kQolErrNotReady, "not in game", 11);
    if (fn_set_stat_base == nullptr || fn_get_stat_base == nullptr) {
        return contracts::error(kQolErrUnsupported, "symbol not resolved", 19);
    }
    uint8_t* flag = *reinterpret_cast<uint8_t**>(g_base + G_STATUSDICE_FLAG_GOT_VMA);
    if (flag == nullptr || (*flag & 1u) == 0) {
        return contracts::error(kQolErrNotFound, "no dice result pending", 22);
    }
    void* leader = member_or_null(0);
    if (leader == nullptr) return contracts::error(kQolErrNotFound, "no leader", 9);
    int8_t* pending = *reinterpret_cast<int8_t**>(g_base + G_STATUSDICE_PENDING_GOT_VMA);
    if (pending == nullptr) return contracts::error(kQolErrInternal, "dice result missing", 19);
    // 骰子已在掷骰时消耗，accept 只应用结果不重复消耗。
    int32_t base[contracts::kQolItemDiceStatCount];
    for (uint32_t i = 0; i < contracts::kQolItemDiceStatCount; ++i) {
        base[i] = fn_get_stat_base(leader, static_cast<int>(i));
    }
    for (uint32_t i = 0; i < contracts::kQolItemDiceStatCount; ++i) {
        fn_set_stat_base(leader, static_cast<int>(i), pending[i]);
    }
    *flag &= ~1u;
    for (uint32_t i = 0; i < contracts::kQolItemDiceStatCount; ++i) {
        out->base[i] = base[i];
        out->applied[i] = pending[i];
        out->delta[i] = pending[i] - base[i];
    }
    return contracts::ok();
}

}  // namespace qol::game::ops
