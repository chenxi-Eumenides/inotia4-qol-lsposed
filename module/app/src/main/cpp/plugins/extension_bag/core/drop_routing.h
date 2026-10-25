#pragma once

#include <cstdint>

#include "contracts/item_handle.h"
#include "plugins/extension_bag/runtime/reentry_lock.h"

bool resolve_original_drag_source(void* moving_control, int* out_bag, int* out_slot,
                                  qol::contracts::QolItemHandle* out_expected_item);
bool route_projected_session_to_tab_locked(::qol::extension_bag::ReentryLock& lock,
                                           int target_bag);
bool route_deferred_extension_to_original_locked(::qol::extension_bag::ReentryLock& lock);
bool route_projected_session_drop_locked(::qol::extension_bag::ReentryLock& lock,
                                         int64_t x, int64_t y);
bool handle_bag_drop_release_locked(::qol::extension_bag::ReentryLock& lock,
                                    int64_t x, int64_t y);
