#include "plugins/extension_bag/runtime/ui_ports.h"

namespace {

SyncSlotFn g_sync_slot = nullptr;
ResetDragFn g_reset_drag = nullptr;

}  // namespace

void set_ui_ports(SyncSlotFn sync, ResetDragFn reset) {
    g_sync_slot = sync;
    g_reset_drag = reset;
}

bool sync_slot_via_port(int bag, int slot) {
    return g_sync_slot != nullptr && g_sync_slot(bag, slot);
}

void reset_drag_via_port(void* control) {
    if (g_reset_drag != nullptr) g_reset_drag(control);
}
