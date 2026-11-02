#pragma once

#include "plugins/extension_bag/runtime/ownership_port.h"

#include <cstdint>

struct ModuleObjectSlotPort {
    void*& item;
    ModuleUseState& use;
    int& category;
    uint32_t& hash;
    uint32_t& handle;
};

struct ModuleObjectSlotSnapshot {
    void* item = nullptr;
    ModuleUseState use{};
    int category = 0;
    uint32_t hash = 0;
    uint32_t handle = 0;
};

struct ModuleViewStatePort {
    bool installed = false;
    int index = -1;
    int window_original_bag = -1;
    int original_bag_size_bag = -1;
    uint32_t original_bag_size = 0;
    uint8_t original_current_direct = 0;
    uint8_t original_current_got = 0;
};

ModuleObjectSlotPort module_object_slot_port(int bag, int slot);
ModuleObjectSlotSnapshot module_object_slot_snapshot(int bag, int slot);
void restore_module_object_slot(int bag, int slot, const ModuleObjectSlotSnapshot& snapshot);
void clear_module_object_bag_state(int bag);
ModuleViewStatePort module_view_state_port();
bool module_view_installed_port();
bool inventory_frame_active_port();
bool extension_drag_active_port();
bool extension_touch_capture_active_port();
bool extension_drag_session_active_port();

// These adapters keep projected-control pointers inside core/.
bool set_projected_slot_item_port(int bag, int slot, void* item);
bool projected_slot_matches_port(int bag, int slot, void* item);

void* module_item_locked(int bag, int slot);

void refresh_module_item_area_locked(int index);

void restore_module_view_locked();
