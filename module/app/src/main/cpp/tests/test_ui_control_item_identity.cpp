#include <array>
#include <cassert>
#include <cstdint>
#include <cstring>

#include "contracts/result.h"
#include "game/access/game_access.h"
#include "game/access/inventory.h"
#include "game/access/item_handle_registry.h"
#include "game/access/ui_kit.h"
#include "game/adapter_ports/game_host_port.h"

namespace {

struct FakeControl {
    alignas(void*) uint8_t bytes[0x80]{};
    void* item = nullptr;
    int32_t user_type = 2;
};

struct RegisteredItem {
    void* pointer = nullptr;
    qol::contracts::QolItemHandle handle{};
    uint32_t owner = 0;
};

std::array<void*, 96> g_physical_items{};
std::array<RegisteredItem, 8> g_registered{};
int g_physical_reads = 0;
int g_register_calls = 0;
int g_fail_physical_read = -1;
int g_registry_failure = 0;
bool g_main_thread = true;
bool g_memory_accessible = true;
FakeControl* g_control_for_callbacks = nullptr;

void set_control_proc(void*, uintptr_t) {}
void set_execute_proc(void*, UiClickProc) {}

}  // namespace

uintptr_t g_base = 0;

bool game_memory_accessible(const void* address, size_t, char) {
    return address != nullptr && g_memory_accessible;
}

namespace qol::game {

bool game_host_is_main_thread() { return g_main_thread; }

uint32_t control_user_type(void* ctrl) {
    return static_cast<FakeControl*>(ctrl)->user_type;
}

void* control_item(void* ctrl) { return static_cast<FakeControl*>(ctrl)->item; }

contracts::QolItemHandle lookup_item(void* item, uint32_t* owner) {
    if (owner != nullptr) *owner = 0;
    for (const RegisteredItem& entry : g_registered) {
        if (entry.pointer == item) {
            if (owner != nullptr) *owner = entry.owner;
            return entry.handle;
        }
    }
    return contracts::kInvalidItemHandle;
}

contracts::QolItemHandle register_item(void* item, uint32_t owner, ItemHandleError* error) {
    ++g_register_calls;
    if (g_registry_failure != 0) {
        if (error != nullptr) *error = ItemHandleError::kFull;
        return contracts::kInvalidItemHandle;
    }
    for (RegisteredItem& entry : g_registered) {
        if (entry.pointer == nullptr) {
            entry = {item, {static_cast<uint32_t>(&entry - g_registered.data() + 1), 1}, owner};
            if (error != nullptr) *error = ItemHandleError::kNone;
            return entry.handle;
        }
    }
    if (error != nullptr) *error = ItemHandleError::kFull;
    return contracts::kInvalidItemHandle;
}

QolResult physical_item_at(int32_t bag, int32_t slot, void** out_item) {
    const int index = bag * 16 + slot;
    ++g_physical_reads;
    if (out_item == nullptr || bag < 0 || bag >= 6 || slot < 0 || slot >= 16) {
        return contracts::error(kQolErrInvalidArgument, "invalid", 7);
    }
    *out_item = nullptr;
    if (index == g_fail_physical_read) {
        return contracts::error(kQolErrNotReady, "read failure", 12);
    }
    *out_item = g_physical_items[static_cast<size_t>(index)];
    return *out_item == nullptr ? contracts::error(kQolErrNotFound, "empty", 5)
                                : contracts::ok();
}

}  // namespace qol::game

ControlObjectGetUserTypeFn fn_control_object_get_user_type = [](void* ctrl) {
    return static_cast<uint32_t>(static_cast<FakeControl*>(ctrl)->user_type);
};
ControlItemGetItemFn fn_control_item_get_item = [](void* ctrl) -> void* {
    return static_cast<FakeControl*>(ctrl)->item;
};

#include "game/access/ui_kit_controls.inc"

int main() {
    alignas(16) uint8_t item[0x20]{};
    FakeControl control{};
    control.item = item;
    g_physical_items[95] = item;
    qol::contracts::QolItemHandle out{99, 99};

    assert(ui_control_item_identity(&control, &out));
    assert(out.slot != 0 && out.generation == 1);
    assert(g_register_calls == 1);
    assert(g_physical_reads == 96);
    assert(g_registered[0].owner == qol::contracts::kQolItemHandleOwnerGame);

    const auto preserved = out;
    assert(ui_control_item_identity(&control, &out));
    assert(out.slot == preserved.slot && g_register_calls == 1);
    assert(g_physical_reads == 96);

    // A previously registered provider item stays with its provider after it disappears
    // from the registry's active view; identity lookup never rebinds it to Game.
    alignas(16) uint8_t provider_item[0x20]{};
    g_registered[1] = {provider_item, {50, 7}, 0x1234};
    control.item = provider_item;
    out = {99, 99};
    assert(ui_control_item_identity(&control, &out));
    assert(out.slot == 50 && out.generation == 7);
    assert(g_register_calls == 1);

    // The control pointer itself is not proof of original inventory membership.
    alignas(16) uint8_t foreign_item[0x20]{};
    control.item = foreign_item;
    out = {99, 99};
    assert(!ui_control_item_identity(&control, &out));
    assert(out.slot == 99 && out.generation == 99 && g_register_calls == 1);

    // Any low-level read failure makes the complete physical inventory scan inconclusive.
    g_fail_physical_read = 5;
    g_physical_items[80] = foreign_item;
    assert(!ui_control_item_identity(&control, &out));
    assert(out.slot == 99 && out.generation == 99 && g_register_calls == 1);
    g_fail_physical_read = -1;

    // Native drag can come from any original bag, including task bag 5.
    control.item = foreign_item;
    g_physical_items[5 * 16 + 15] = foreign_item;
    assert(ui_control_item_identity(&control, &out));
    assert(out.slot != 0 && g_registered[2].owner == qol::contracts::kQolItemHandleOwnerGame);

    // Registration failure does not publish a partial output handle.
    alignas(16) uint8_t another_item[0x20]{};
    control.item = another_item;
    g_physical_items[0] = another_item;
    g_registry_failure = 1;
    out = {88, 88};
    assert(!ui_control_item_identity(&control, &out));
    assert(out.slot == 88 && out.generation == 88);
}
