#pragma once

#include <cstdint>

bool p5_observation_enabled();
bool p5_panel_observation_enabled();
bool p5_should_begin_item_observation(uint64_t event);
uint64_t log_p5_observation_locked(const char* phase, void* target_control, uint64_t event,
                                   bool x2_present, bool param_present, void* source_control,
                                   bool result_known, uint64_t result, int64_t event_x = -1,
                                   int64_t event_y = -1, uint64_t related_sequence = 0);
void log_exit_trace_locked(const char* phase, uint64_t event, uint64_t param,
                            uint64_t param2, int64_t x = -1, int64_t y = -1);
