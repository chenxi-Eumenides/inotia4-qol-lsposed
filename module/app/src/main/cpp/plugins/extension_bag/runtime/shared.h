#pragma once

#include "plugins/extension_bag/runtime/state.h"
#include "plugins/extension_bag/runtime/session.h"
#include "plugins/extension_bag/runtime/response.h"

#include <string>

// State revisions, diagnostics, clocks, JNI access, and public bridges span runtime modules.
// They stay shared because they do not have one narrower runtime owner.
void extension_bag_mark_dirty_locked();
uint64_t extension_bag_item_revision_locked();
bool extension_bag_invalidate_game_item_handle(void* item);
bool original_inventory_contains_cached_locked(void* item);
uint64_t extension_bag_ui_isolation_now_ms_locked();
JNIEnv* current_env();
uint64_t isolation_now_ms_locked();
