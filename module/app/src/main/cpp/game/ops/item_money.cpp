#include "game/ops/item_money.h"

#include "contracts/game_cache.h"
#include "game/access/game_access.h"
#include "game/state/game_state.h"

namespace qol::game::ops {

QolResult set_money(int64_t money) {
    if (!game_in_world()) return contracts::error(kQolErrNotReady, "not in game", 11);
    if (fn_set_money == nullptr) return contracts::error(kQolErrUnsupported, "symbol not resolved", 19);

    fn_set_money(money);
    frame_cache_force_refresh();
    return contracts::ok();
}

QolResult add_money(int64_t delta) {
    if (!game_in_world()) return contracts::error(kQolErrNotReady, "not in game", 11);
    if (fn_add_money == nullptr) return contracts::error(kQolErrUnsupported, "symbol not resolved", 19);

    const int r = fn_add_money(delta);
    if (!r) return contracts::error(kQolErrInternal, "add money failed", 16);
    frame_cache_force_refresh();
    return contracts::ok();
}

QolResult minus_money(int64_t delta) {
    if (!game_in_world()) return contracts::error(kQolErrNotReady, "not in game", 11);
    if (fn_minus_money == nullptr) {
        return contracts::error(kQolErrUnsupported, "symbol not resolved", 19);
    }

    const int r = fn_minus_money(delta);
    if (!r) return contracts::error(kQolErrInternal, "insufficient money", 18);
    frame_cache_force_refresh();
    return contracts::ok();
}

}  // namespace qol::game::ops
