#pragma once

#include "plugins/extension_bag/runtime/state.h"

int extension_tab_index(void* ctrl);
void refresh_tab_bag_items_locked();
void begin_projected_drag_session_locked(int source_slot);
void advance_projected_drag_session_locked();
bool projected_drag_session_active_locked();
void cancel_projected_drag_session_locked(const char* reason);
void* touch_moving_item_control_locked();
void reset_drag_state_locked(void* moving_control);
void clear_stale_projected_moving_locked();
void complete_original_release_cleanup_locked(void* moving_control, int64_t rel_x, int64_t rel_y,
                                              uint64_t release_param2, bool handled);
void clear_original_item_selection_locked();
int original_item_category_locked(void* item);
int drag_source_category_locked();
void commit_pending_extension_tab_locked(std::unique_lock<std::mutex>& lock);
void reset_extension_desc_unequip_hook_locked();
void reset_extension_desc_item_hooks_locked();
void install_extension_desc_item_hooks_locked(void* item);
void extension_desc_item_execute_at(size_t index, void* button);
bool extension_desc_original_matches(uint64_t id, const char* symbol);
bool extension_desc_call_original(uint64_t id, void* button);
void restore_extension_desc_popup_callbacks_locked();
void delete_extension_tab_buttons_locked();
void disable_extension_tab_buttons_locked();
void install_extension_tab_buttons_locked();
bool extension_tab_buttons_complete_locked();
bool extension_tab_hit(int index, int64_t x, int64_t y);
bool try_equip_on_extension_tab_drop_locked(std::unique_lock<std::mutex>& lock,
                                             int index, void* moving_control);
bool extension_source_should_equip_locked(int target_bag);
bool equip_extension_source_on_tab_locked(std::unique_lock<std::mutex>& lock, int target_bag,
                                          int source_bag, int source_slot);
void extension_bag_ui_handle_tab_click_locked(int extension_bag);
void handle_extension_tab_click_locked(int extension_bag);
