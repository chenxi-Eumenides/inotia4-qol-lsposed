#pragma once

#include "plugins/extension_bag/runtime/state.h"

#include <string>

// State revisions, diagnostics, clocks, JNI access, and public bridges span runtime modules.
// They stay shared because they do not have one narrower runtime owner.
void extension_bag_mark_dirty_locked();
uint64_t extension_bag_item_revision_locked();
bool extension_bag_invalidate_game_item_handle(void* item);
bool p5_observation_enabled();
bool p5_panel_observation_enabled();
bool p5_should_begin_item_observation(uint64_t event);
uint64_t log_p5_observation_locked(const char* phase, void* target_control, uint64_t event,
                                   bool x2_present, bool param_present, void* source_control,
                                   bool result_known, uint64_t result, int64_t event_x = -1,
                                   int64_t event_y = -1, uint64_t related_sequence = 0);
void log_exit_trace_locked(const char* phase, uint64_t event, uint64_t param,
                           uint64_t param2, int64_t x, int64_t y);
bool original_inventory_contains_cached_locked(void* item);
uint64_t extension_bag_ui_isolation_now_ms_locked();
JNIEnv* current_env();
uint64_t isolation_now_ms_locked();
std::string extension_bag_status_json_locked_bridge();
std::string extension_bag_not_ready_error_locked_bridge();
bool extension_bag_ready_locked_bridge();
int extension_internal_bag_bridge(int logical_bag);
std::string extension_bag_view_result_json_bridge(bool ok, const char* error);
