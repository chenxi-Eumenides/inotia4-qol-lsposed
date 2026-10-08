#pragma once

namespace qol::features::extension_bag::model {

// Game state 4 is the main menu; state 5 is world. Popup state is not a cleanup signal.
inline bool main_menu_cleanup_due(int previous_world_state, int current_world_state,
                                  bool world_state_known, bool retry_dirty) {
    if (!world_state_known || current_world_state < 0 || current_world_state == 5) return false;
    const bool entered_main_menu = current_world_state == 4 && previous_world_state != 4;
    const bool left_world = previous_world_state == 5 && current_world_state != 5;
    return retry_dirty || entered_main_menu || left_world;
}

}  // namespace qol::features::extension_bag::model
