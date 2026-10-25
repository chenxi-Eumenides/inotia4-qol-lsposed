#include "plugins/extension_bag/runtime/object_state_port.h"

#include "plugins/extension_bag/core/geometry.h"
#include "plugins/extension_bag/core/ui_state.h"
#include "plugins/extension_bag/runtime/ui_ports.h"
#include "plugins/extension_bag/transaction/transaction_ports.h"

#include <algorithm>

ModuleObjectSlotPort module_object_slot_port(int bag, int slot) {
    auto& objects = qol::extension_bag::ui_state().objects;
    return {objects.module_objects[bag][slot], objects.module_object_use[bag][slot],
            objects.module_object_categories[bag][slot], objects.module_object_hashes[bag][slot],
            objects.module_object_handles[bag][slot]};
}

ModuleObjectSlotSnapshot module_object_slot_snapshot(int bag, int slot) {
    const auto& objects = qol::extension_bag::ui_state().objects;
    return {objects.module_objects[bag][slot], objects.module_object_use[bag][slot],
            objects.module_object_categories[bag][slot], objects.module_object_hashes[bag][slot],
            objects.module_object_handles[bag][slot]};
}

void restore_module_object_slot(int bag, int slot, const ModuleObjectSlotSnapshot& snapshot) {
    auto state = module_object_slot_port(bag, slot);
    state.item = snapshot.item;
    state.use = snapshot.use;
    state.category = snapshot.category;
    state.hash = snapshot.hash;
    state.handle = snapshot.handle;
}

void clear_module_object_bag_state(int bag) {
    auto& objects = qol::extension_bag::ui_state().objects;
    objects.module_objects[bag] = {};
    objects.module_object_use[bag] = {};
    objects.module_object_categories[bag] = {};
    objects.module_object_hashes[bag] = {};
    std::fill(std::begin(objects.module_object_handles[bag]),
              std::end(objects.module_object_handles[bag]), 0u);
}

ModuleViewStatePort module_view_state_port() {
    const auto& objects = qol::extension_bag::ui_state().objects;
    return {objects.module_view_installed, objects.module_view_index,
            objects.module_window_original_bag, objects.original_bag_size_bag,
            objects.original_bag_size, objects.original_current_direct,
            objects.original_current_got};
}

bool module_view_installed_port() {
    return qol::extension_bag::ui_state().objects.module_view_installed;
}

bool inventory_frame_active_port() {
    return qol::extension_bag::ui_state().ui.inventory_frame_active;
}

bool extension_drag_active_port() {
    return qol::extension_bag::ui_state().ui.extension_drag.active;
}

bool extension_touch_capture_active_port() {
    return qol::extension_bag::ui_state().ui.extension_touch_capture;
}

bool extension_drag_session_active_port() {
    return qol::extension_bag::ui_state().ui.extension_drag_session.phase !=
           virtual_bag::DragPhase::kIdle;
}

bool set_projected_slot_item_port(int bag, int slot, void* item) {
    const auto& objects = qol::extension_bag::ui_state().objects;
    if (!objects.module_view_installed || objects.module_view_index != bag) return true;
    if (objects.projected_item_root == nullptr) return false;
    void* control = valid_child_locked(objects.projected_item_root, slot);
    return control != nullptr && extension_bag_ui_set_control_item(control, item);
}

bool projected_slot_matches_port(int bag, int slot, void* item) {
    const auto& objects = qol::extension_bag::ui_state().objects;
    if (!objects.module_view_installed || objects.module_view_index != bag ||
        objects.projected_item_root == nullptr) {
        return false;
    }
    void* control = valid_child_locked(objects.projected_item_root, slot);
    if (control == nullptr || !sync_slot_via_port(bag, slot)) return false;
    void* data = extension_bag_ui_data(control);
    return data != nullptr && *reinterpret_cast<void**>(data) == item;
}
