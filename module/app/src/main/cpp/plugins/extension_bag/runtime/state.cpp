#include "plugins/extension_bag/extension_bag_internal.h"
#include "plugins/extension_bag/runtime/bag_state_store.h"

#include "plugins/extension_bag/extension_bag_context.h"
#include "plugins/extension_bag/extension_bag_geometry.h"

int virtual_bag::active_extension_bags() {
    return virtual_bag::clamp_active_extension_bags(runtime_state().objects.bag.bag_count);
}

int virtual_bag::extension_logical_bag_last() {
    return virtual_bag::kExtensionLogicalBagFirst + virtual_bag::active_extension_bags() - 1;
}

thread_local ModuleUseToken* g_active_module_use_token = nullptr;
thread_local bool g_p5_observation_active = false;
thread_local bool g_refresh_restore_suppress = false;
thread_local bool g_in_native_call = false;

void extension_bag_mark_dirty_locked() {
    runtime_state().persistence.dirty = true;
    runtime_state().persistence.revision.fetch_add(1, std::memory_order_release);
    // 主线程写路径统一在这里发布轻量快照，读侧可无锁取一致视图。
    qol::extension_bag::publish_bag_snapshot();
}

uint64_t extension_bag_item_revision_locked() {
    return runtime_state().persistence.revision.load(std::memory_order_acquire);
}

// G-8/H4 共享扩展身份判据：所有事件层与函数层守卫都反查同一份
// g_module_objects，避免 source 判定在不同防线间漂移。调用方必须已持有
// g_virtual_bag_mtx。
bool module_slot_of_item_locked(void* item, int* out_bag, int* out_slot) {
    if (item == nullptr || out_bag == nullptr || out_slot == nullptr) return false;
    for (int bag = 0; bag < virtual_bag::active_extension_bags(); ++bag) {
        for (int slot = 0; slot < virtual_bag::kSlotCount; ++slot) {
            if (runtime_state().objects.module_objects[bag][slot] == item) {
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
