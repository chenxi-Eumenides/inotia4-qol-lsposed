#pragma once

#include <cstdint>
#include <mutex>

extern std::mutex g_virtual_bag_mtx;
extern bool g_item_state_dirty;
extern int64_t g_item_state_shortcut_refreshed_revision;
