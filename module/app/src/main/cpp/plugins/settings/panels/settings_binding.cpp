#include "settings_binding.h"

namespace qol::features::core_ui {

void more_games_bind_tick(bool on_main_menu, MoreGamesBindOps& ops) {
    if (!on_main_menu) {
        ops.mark_waiting();
        return;
    }
    const MoreGamesBindingState state = ops.binding_state();
    if (state == MoreGamesBindingState::kUnknown) {
        ops.mark_waiting();
        return;
    }
    if (state == MoreGamesBindingState::kLive) {
        ops.mark_live();
        return;
    }
    // Only replace after the old native callback owner has been safely released.
    if (!ops.release()) {
        ops.mark_waiting();
        return;
    }
    if (ops.bind()) {
        ops.mark_live();
    } else {
        ops.mark_failed(ops.btn_resolvable());
    }
}

bool more_games_draw(bool feature_enabled, MoreGamesDrawOps& ops) {
    if (feature_enabled && ops.replacement_live() && ops.draw_replacement()) return true;
    ops.draw_original();
    return false;
}

bool more_games_disable_original(MoreGamesOriginalStateOps& ops, bool original_active) {
    const bool shown = ops.set_show(false);
    const bool enabled = shown && ops.set_enabled(false);
    const bool active = enabled && ops.set_active(false);
    if (shown && enabled && active) return true;
    (void)ops.set_show(true);
    (void)ops.set_enabled(true);
    (void)ops.set_active(original_active);
    return false;
}

bool more_games_restore_original(bool original_is_current, bool original_active,
                                 MoreGamesOriginalStateOps& ops) {
    if (!original_is_current) return true;
    const bool shown = ops.set_show(true);
    const bool enabled = ops.set_enabled(true);
    const bool active = ops.set_active(original_active);
    return shown && enabled && active;
}

}  // namespace qol::features::core_ui
