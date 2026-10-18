#include "plugins/extension_bag/extension_bag_internal.h"

#include "plugins/extension_bag/extension_bag_context.h"
#include "plugins/extension_bag/extension_bag_geometry.h"

thread_local ModuleUseToken* g_active_module_use_token = nullptr;
thread_local bool g_p5_observation_active = false;
thread_local bool g_refresh_restore_suppress = false;
thread_local bool g_in_native_call = false;

void extension_bag_mark_dirty_locked() {
    g_item_state_dirty = true;
    g_item_state_revision.fetch_add(1, std::memory_order_release);
}

uint64_t extension_bag_item_revision_locked() {
    return g_item_state_revision.load(std::memory_order_acquire);
}

const qol::contracts::QolExtensionBagWritePort* extension_bag_write_api() { return g_write_port; }

bool read_item_category(const void* item, int* out_category) {
    qol::contracts::QolItemHandle handle{};
    if (g_item_facts == nullptr || g_item_facts->category == nullptr ||
        item == nullptr || out_category == nullptr ||
        !extension_bag_item_handle_for_pointer(const_cast<void*>(item), &handle)) return false;
    int32_t category = 0;
    if (!qol::contracts::succeeded(
            g_item_facts->category(handle, &category))) return false;
    *out_category = category;
    return true;
}

void* extension_create_item(int category, int arg1, int arg2, int arg3) {
    const auto* api = extension_bag_write_api();
    if (api == nullptr || api->inventory.create_item == nullptr) return nullptr;
    void* item = nullptr;
    return qol::contracts::succeeded(
               api->inventory.create_item(category, arg1, arg2, arg3, &item))
               ? item
               : nullptr;
}

const qol::contracts::QolItemCountApi* item_count_api() {
    return g_game == nullptr ? nullptr : &g_game->item_count;
}

bool mark_native_bag_object(void* item) {
    const auto* api = item_count_api();
    if (api == nullptr || api->read_encoded_field == nullptr ||
        api->write_encoded_field == nullptr || item == nullptr) return false;
    uint32_t field = 0;
    if (!qol::contracts::succeeded(api->read_encoded_field(item, &field))) return false;
    field = qol::features::extension_bag::write_native_bag_object_marker(field);
    return qol::contracts::succeeded(api->write_encoded_field(item, field));
}

// G-8/H4 共享扩展身份判据：所有事件层与函数层守卫都反查同一份
// g_module_objects，避免 source 判定在不同防线间漂移。调用方必须已持有
// g_virtual_bag_mtx。
bool module_slot_of_item_locked(void* item, int* out_bag, int* out_slot) {
    if (item == nullptr || out_bag == nullptr || out_slot == nullptr) return false;
    for (int bag = 0; bag < virtual_bag::kBagCount; ++bag) {
        for (int slot = 0; slot < virtual_bag::kSlotCount; ++slot) {
            if (g_module_objects[bag][slot] == item) {
                *out_bag = bag;
                *out_slot = slot;
                return true;
            }
        }
    }
    return false;
}

// GetChild 结果必须过双重校验：面板重建窗口里 GetChild(slot≥count) 返回不可读的
// 垃圾指针（tombstone_02），先 GetCount 判界消除越界源，再 GetUserType==2 验类型。
// root 本身也可能被面板重建替换——调用方应实时通过 UI adapter 查询，
// 不要使用 install 时的快照。
void* valid_child_locked(void* root, int slot) {
    return extension_bag_valid_child(root, slot);
}
