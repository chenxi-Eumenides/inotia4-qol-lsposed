#include "settings_binding.h"

#include <cstring>

namespace qol::features::core_ui {

void more_games_bind_tick(bool on_main_menu, MoreGamesBindOps& ops) {
    if (!on_main_menu) {
        ops.release();
        return;
    }
    if (ops.proc_matches()) {
        ops.mark_live();
        return;
    }
    // 无 token（幂等复位）或槽已被游戏改写/记录失效：先释放记录再重装。
    ops.release();
    if (ops.bind()) {
        ops.mark_live();
    } else {
        ops.mark_failed(ops.btn_resolvable());
    }
}

}  // namespace qol::features::core_ui
