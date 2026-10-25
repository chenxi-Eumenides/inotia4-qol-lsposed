#pragma once

#include "plugins/extension_bag/runtime/state.h"
#include "plugins/extension_bag/runtime/reentry_lock.h"
#include "plugins/extension_bag/core/drag_session.h"

int extension_tab_index(void* ctrl);
void refresh_tab_bag_items_locked();
void commit_pending_extension_tab_locked(::qol::extension_bag::ReentryLock& lock);
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
// C18 改版客户端追加袋页签命中：返回原生袋号（= 袋容器子控件序号，与原生
// UIEquip_GetBagSlotIndex 同一定义）。歧义/未命中返回 false。
bool extension_c18_native_bag_hit(int64_t x, int64_t y, int* out_native_bag);
bool try_equip_on_extension_tab_drop_locked(::qol::extension_bag::ReentryLock& lock,
                                             int index, void* moving_control);
bool extension_source_should_equip_locked(int target_bag);
bool equip_extension_source_on_tab_locked(::qol::extension_bag::ReentryLock& lock, int target_bag,
                                          int source_bag, int source_slot);
void extension_bag_ui_handle_tab_click_locked(int extension_bag);
void handle_extension_tab_click_locked(int extension_bag);
