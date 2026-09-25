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

}  // namespace qol::game::ops
