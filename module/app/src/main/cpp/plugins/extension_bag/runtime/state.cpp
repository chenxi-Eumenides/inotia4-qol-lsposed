#include "plugins/extension_bag/runtime/shared.h"
#include "plugins/extension_bag/runtime/state.h"
#include "plugins/extension_bag/runtime/bag_state_store.h"

int virtual_bag::active_extension_bags() {
    return virtual_bag::clamp_active_extension_bags(runtime_state().objects.bag.bag_count);
}

int virtual_bag::extension_logical_bag_last() {
    return virtual_bag::kExtensionLogicalBagFirst + virtual_bag::active_extension_bags() - 1;
}

virtual_bag::State* extension_bag_state() { return &runtime_state().objects.bag; }

thread_local bool g_p5_observation_active = false;
thread_local bool g_refresh_restore_suppress = false;
thread_local bool g_in_native_call = false;

void extension_bag_mark_dirty_locked() {
    runtime_state().persistence.dirty = true;
    runtime_state().persistence.revision.fetch_add(1, std::memory_order_release);
    // 主线程写路径统一在这里发布轻量袋快照和完整状态查询快照。
    qol::extension_bag::publish_extension_bag_snapshot();
}

uint64_t extension_bag_item_revision_locked() {
    return runtime_state().persistence.revision.load(std::memory_order_acquire);
}

// GetChild 结果必须过双重校验：面板重建窗口里 GetChild(slot≥count) 返回不可读的
// 垃圾指针（tombstone_02），先 GetCount 判界消除越界源，再 GetUserType==2 验类型。
// root 本身也可能被面板重建替换——调用方应实时通过 UI adapter 查询，
// 不要使用 install 时的快照。
