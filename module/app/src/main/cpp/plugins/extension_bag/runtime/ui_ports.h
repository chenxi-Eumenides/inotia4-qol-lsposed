#pragma once

using SyncSlotFn = bool (*)(int, int);
using ResetDragFn = void (*)(void*);

void set_ui_ports(SyncSlotFn sync, ResetDragFn reset);
bool sync_slot_via_port(int bag, int slot);
void reset_drag_via_port(void* control);
