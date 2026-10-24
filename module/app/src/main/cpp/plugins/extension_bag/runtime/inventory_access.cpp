#include "plugins/extension_bag/runtime/inventory_access.h"
#include "plugins/extension_bag/runtime/reentry_lock.h"

#include "plugins/extension_bag/extension_bag_port.h"
#include "plugins/extension_bag/plugin.h"
#include "plugins/extension_bag/runtime/projection.h"

#include <mutex>

#define extension_remove_item_direct extension_remove_item_direct_internal
#define extension_refresh_bag_area extension_refresh_bag_area_internal

const qol::contracts::QolExtensionBagWritePort* extension_bag_write_api() { return runtime_state().host.write_port; }

bool extension_save_item_on_empty(void* item, int bag) {
    const auto* api = extension_bag_write_api();
    if (api == nullptr || api->inventory.save_item_on_empty == nullptr) return false;
    int32_t result = 0;
    return qol::contracts::succeeded(api->inventory.save_item_on_empty(item, bag, &result)) &&
           result != 0;
}

bool extension_move_item(void* item, int count, int bag, int slot, int* out_result) {
    const auto* api = extension_bag_write_api();
    if (api == nullptr || api->inventory.move_item == nullptr || out_result == nullptr) return false;
    int32_t result = 0;
    const QolResult call = api->inventory.move_item(item, count, bag, slot, &result);
    if (!qol::contracts::succeeded(call)) return false;
    *out_result = result;
    return true;
}

namespace {

bool extension_remove_item_direct(int bag, int slot, int* out_result) {
    const auto* api = extension_bag_write_api();
    if (api == nullptr || api->inventory.remove_item_direct == nullptr || out_result == nullptr) {
        return false;
    }
    int32_t result = 0;
    const QolResult call = api->inventory.remove_item_direct(bag, slot, &result);
    if (!qol::contracts::succeeded(call)) return false;
    *out_result = result;
    return true;
}

}  // namespace

void extension_refresh_item_area() {
    const auto* api = extension_bag_write_api();
    if (api != nullptr && api->ui.refresh_item_area != nullptr) {
        (void)api->ui.refresh_item_area();
    }
}

namespace {

void extension_refresh_bag_area() {
    const auto* api = extension_bag_write_api();
    if (api != nullptr && api->ui.refresh_bag_area != nullptr) {
        (void)api->ui.refresh_bag_area();
    }
}

}  // namespace

namespace {

bool read_inventory_bag_size(int bag, int* out_size) {
    if (runtime_state().host.game == nullptr || runtime_state().host.inventory_extras == nullptr || runtime_state().host.inventory_extras->read.bag_size == nullptr ||
        out_size == nullptr) return false;
    int32_t value = 0;
    if (!qol::contracts::succeeded(runtime_state().host.inventory_extras->read.bag_size(bag, &value))) return false;
    *out_size = value;
    return true;
}

bool read_inventory_is_empty_bag(int bag) {
    if (runtime_state().host.game == nullptr || runtime_state().host.inventory_extras == nullptr || runtime_state().host.inventory_extras->read.is_empty_bag == nullptr) return false;
    int32_t value = 0;
    return qol::contracts::succeeded(
               runtime_state().host.inventory_extras->read.is_empty_bag(bag, &value)) && value != 0;
}

bool read_inventory_find_save_slot(const void* item, int* out_slot) {
    if (runtime_state().host.game == nullptr || runtime_state().host.inventory_extras == nullptr || runtime_state().host.inventory_extras->read.find_save_slot == nullptr ||
        item == nullptr || out_slot == nullptr) return false;
    int32_t value = -1;
    if (!qol::contracts::succeeded(
            runtime_state().host.inventory_extras->read.find_save_slot(item, &value))) return false;
    *out_slot = value;
    return true;
}

int read_current_save_slot() {
    const auto* facts = qol::features::extension_bag::save_facts();
    if (facts == nullptr || facts->current_slot == nullptr) return -1;
    int32_t slot = -1;
    return qol::contracts::succeeded(facts->current_slot(&slot)) ? slot : -1;
}

}  // namespace

bool read_world_state_id(int* out_state) {
    if (out_state == nullptr) return false;
    const auto* facts = qol::features::extension_bag::world_facts();
    if (facts == nullptr || facts->state == nullptr) return false;
    int32_t state = -1;
    if (!qol::contracts::succeeded(facts->state(&state))) return false;
    *out_state = state;
    return true;
}

bool read_world_in_world() {
    int state = -1;
    return read_world_state_id(&state) && state == 5;
}

int raw_direct_bag_locked() {
    if (runtime_state().host.game == nullptr || runtime_state().host.write_port->ui.current_bag == nullptr) return -1;
    int32_t bag = -1;
    return qol::contracts::succeeded(runtime_state().host.write_port->ui.current_bag(&bag)) ? bag : -1;
}

int raw_got_bag_locked() {
    return raw_direct_bag_locked();
}

int raw_desc_type_locked() {
    if (runtime_state().host.game == nullptr || runtime_state().host.write_port->ui.desc_type == nullptr) return -1;
    int32_t type = -1;
    return qol::contracts::succeeded(runtime_state().host.write_port->ui.desc_type(&type)) ? type : -1;
}

bool inventory_slot_locked(int bag, int slot, void** out) {
    // C18 改版页的原生追加袋（6..N-1）也要能作为移动来源读取。
    // 只读物理槽：原版 0..5（含任务袋）保持可读；C18 追加袋仅在候选版本放开。
    if (runtime_state().host.item_facts == nullptr || runtime_state().host.item_facts->physical_item_at == nullptr ||
        out == nullptr || slot < 0 || slot >= virtual_bag::kSlotCount || bag < 0 ||
        (bag > virtual_bag::kOriginalTaskBag && !native_source_bag_allowed_locked(bag))) {
        return false;
    }
    *out = nullptr;
    qol::contracts::QolItemHandle handle{};
    if (!qol::contracts::succeeded(runtime_state().host.item_facts->physical_item_at(bag, slot, &handle))) {
        return false;
    }
    // physical_item_at returns a handle that may be game-owned (original inventory)
    // or provider-owned (EB). Private custody only knows the latter, so resolve
    // through the Game Host handle bridge first.
    const auto* game = runtime_state().host.game;
    if (game != nullptr && game->item_abi.native_item_for_handle != nullptr &&
        qol::contracts::succeeded(game->item_abi.native_item_for_handle(handle, out)) &&
        *out != nullptr) {
        return true;
    }
    return extension_bag_private_item_for_handle(handle, out);
}

int virtual_bag_inventory_state_id() {
    // g_state_entry is panel-local state; world lifecycle cleanup uses world_facts separately.
    // The explicit state access is equivalent to std::lock_guard<std::mutex> lock(g_virtual_bag_mtx),
    // g_state_entry == nullptr, and *reinterpret_cast<int32_t*>(g_state_entry).
    ::qol::extension_bag::ReentryLock lock;
    if (runtime_state().persistence.state_entry == nullptr) return -1;
    return *reinterpret_cast<int32_t*>(runtime_state().persistence.state_entry);
}

#undef extension_remove_item_direct
#undef extension_refresh_bag_area

bool extension_remove_item_direct(int bag, int slot, int* out_result) {
    return extension_remove_item_direct_internal(bag, slot, out_result);
}
void extension_refresh_bag_area() { extension_refresh_bag_area_internal(); }
