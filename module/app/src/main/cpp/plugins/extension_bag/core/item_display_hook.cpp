#include "plugins/extension_bag/core/item_display_hook.h"

#include "contracts/ui.h"
#include "plugins/extension_bag/core/geometry.h"
#include "plugins/extension_bag/core/projection.h"
#include "plugins/extension_bag/core/ui_state.h"
#include "plugins/extension_bag/model/virtual_bag_state.h"
#include "plugins/extension_bag/runtime/projection_controller.h"
#include "plugins/extension_bag/runtime/reentry_lock.h"
#include "plugins/extension_bag/runtime/state_access.h"

namespace qol::features::extension_bag {
namespace {

bool g_suppressed = false;
thread_local int g_substitution_depth = 0;

struct SubstitutionDepthScope {
    SubstitutionDepthScope() { ++g_substitution_depth; }
    ~SubstitutionDepthScope() { --g_substitution_depth; }
    SubstitutionDepthScope(const SubstitutionDepthScope&) = delete;
    SubstitutionDepthScope& operator=(const SubstitutionDepthScope&) = delete;
};

struct PendingDisplay {
    bool active = false;
    qol::contracts::QolUiRootKind kind = qol::contracts::QolUiRootKind::kEquipItemRoot;
    int bag = -1;
    int window_bag = -1;
};

PendingDisplay g_pending;

bool projection_context(qol::contracts::QolUiRootKind kind, int* out_bag,
                        int* out_window_bag, void** out_root, int* out_capacity) {
    if (out_bag == nullptr || out_window_bag == nullptr ||
        out_root == nullptr || out_capacity == nullptr) return false;
    if (g_suppressed) return false;

    auto& state = qol::extension_bag::ui_state();
    int bag = -1;
    int window_bag = -1;
    void* root = nullptr;
    if (g_pending.active && g_pending.kind == kind) {
        bag = g_pending.bag;
        window_bag = g_pending.window_bag;
        root = extension_bag_ui_root(kind);
    } else {
        switch (kind) {
            case qol::contracts::QolUiRootKind::kEquipItemRoot:
                if (!state.objects.module_view_installed) return false;
                bag = state.objects.module_view_index;
                window_bag = state.objects.module_window_original_bag;
                root = state.objects.projected_item_root;
                break;
            case qol::contracts::QolUiRootKind::kStoreItemRoot:
                if (!state.store.view_installed) return false;
                bag = state.store.view_bag;
                window_bag = state.store.window_original_bag;
                root = state.store.projected_item_root;
                break;
            case qol::contracts::QolUiRootKind::kMixItemRoot:
                if (!state.mix.view_installed) return false;
                bag = state.mix.view_bag;
                window_bag = state.mix.window_original_bag;
                root = state.mix.projected_item_root;
                break;
            default:
                return false;
        }
    }
    if (!virtual_bag::valid_index(bag) ||
        !virtual_bag::valid_original_transaction_bag(window_bag) || root == nullptr) return false;
    if (!(g_pending.active && g_pending.kind == kind) &&
        extension_bag_ui_root(kind) != root) return false;
    *out_bag = bag;
    *out_window_bag = window_bag;
    *out_root = root;
    *out_capacity = ::qol::extension_bag::access::bag().capacities[bag];
    return true;
}

bool projection_item_for_control(void* control, void** out_item) {
    if (control == nullptr || out_item == nullptr) return false;
    ::qol::extension_bag::ReentryLock lock;
    constexpr qol::contracts::QolUiRootKind kRootKinds[] = {
        qol::contracts::QolUiRootKind::kEquipItemRoot,
        qol::contracts::QolUiRootKind::kStoreItemRoot,
        qol::contracts::QolUiRootKind::kMixItemRoot,
    };
    int kind_index = 0;
    for (qol::contracts::QolUiRootKind kind : kRootKinds) {
        int bag = -1;
        int window_bag = -1;
        int capacity = 0;
        void* root = nullptr;
        const int current_kind = kind_index++;
        if (!projection_context(kind, &bag, &window_bag, &root, &capacity)) continue;
        const int current_bag = raw_current_bag_locked();
        if (current_bag >= 0 && current_bag != window_bag) continue;

        // 投影期间同一 root 的 control→slot 映射稳定；缓存后 ControlItem 每次调用
        // 不再扫描 16 个槽。命中仍用 valid_child_locked 复核，控件重建后自动重扫。
        struct SlotCache {
            void* root = nullptr;
            int bag = -1;
            int capacity = 0;
            void* controls[virtual_bag::kSlotCount]{};
        };
        static thread_local SlotCache s_slot_cache[3];
        SlotCache& cache = s_slot_cache[current_kind];
        const bool cached = cache.root == root && cache.bag == bag && cache.capacity == capacity;
        int slot = -1;
        if (cached) {
            for (int candidate = 0; candidate < virtual_bag::kSlotCount; ++candidate) {
                if (cache.controls[candidate] != control) continue;
                if (valid_child_locked(root, candidate) == control) slot = candidate;
                break;
            }
        }
        if (slot < 0) {
            cache.root = root;
            cache.bag = bag;
            cache.capacity = capacity;
            for (int candidate = 0; candidate < virtual_bag::kSlotCount; ++candidate) {
                cache.controls[candidate] = valid_child_locked(root, candidate);
            }
            for (int candidate = 0; candidate < virtual_bag::kSlotCount; ++candidate) {
                if (cache.controls[candidate] != control) continue;
                slot = candidate;
                break;
            }
        }
        if (slot < 0) continue;
        if (slot >= capacity) {
            *out_item = nullptr;
            return true;
        }
        const virtual_bag::Item& descriptor = ::qol::extension_bag::access::bag().items[bag][slot];
        if (descriptor.category <= 0 || descriptor.count <= 0) {
            *out_item = nullptr;
            return true;
        }
        ::qol::extension_bag::ReentryLock lock;
        *out_item = module_item_locked(bag, slot);
        return true;
    }
    return false;
}

bool projected_capacity_for_bag(int requested_bag, int* out_capacity) {
    if (out_capacity == nullptr) return false;
    ::qol::extension_bag::ReentryLock lock;
    constexpr qol::contracts::QolUiRootKind kRootKinds[] = {
        qol::contracts::QolUiRootKind::kEquipItemRoot,
        qol::contracts::QolUiRootKind::kStoreItemRoot,
        qol::contracts::QolUiRootKind::kMixItemRoot,
    };
    for (qol::contracts::QolUiRootKind kind : kRootKinds) {
        int display_bag = -1;
        int window_bag = -1;
        int capacity = 0;
        void* root = nullptr;
        if (projection_context(kind, &display_bag, &window_bag, &root, &capacity) &&
            requested_bag == window_bag) {
            (void)display_bag;
            (void)root;
            *out_capacity = capacity;
            return true;
        }
    }
    return false;
}

thread_local int g_bag_size_depth = 0;
thread_local int g_bag_size_projection_depth = 0;

struct BagSizeDepthScope {
    BagSizeDepthScope() { ++g_bag_size_depth; }
    ~BagSizeDepthScope() { --g_bag_size_depth; }
    BagSizeDepthScope(const BagSizeDepthScope&) = delete;
    BagSizeDepthScope& operator=(const BagSizeDepthScope&) = delete;
};

}  // namespace

void extension_bag_control_item_set_item(void* control, void* item) {
    using SetItemFn = void (*)(void*, void*);
    auto* backup = reinterpret_cast<SetItemFn>(
        qol::extension_bag::ui_state().ui.backup_control_item_set_item);
    if (backup == nullptr) return;
    if (g_substitution_depth > 0) {
        backup(control, item);
        return;
    }
    SubstitutionDepthScope depth;
    void* substituted = item;
    if (projection_item_for_control(control, &substituted)) {
        backup(control, substituted);
        return;
    }
    backup(control, item);
}

void* extension_bag_control_item_get_item(void* control) {
    using GetItemFn = void* (*)(void*);
    auto* backup = reinterpret_cast<GetItemFn>(
        qol::extension_bag::ui_state().ui.backup_control_item_get_item);
    if (backup == nullptr) return nullptr;
    if (g_substitution_depth > 0) return backup(control);
    SubstitutionDepthScope depth;
    void* substituted = nullptr;
    if (projection_item_for_control(control, &substituted)) return substituted;
    return backup(control);
}

int extension_bag_inven_get_bag_size(int bag) {
    using GetBagSizeFn = int (*)(int);
    auto* backup = reinterpret_cast<GetBagSizeFn>(
        qol::extension_bag::ui_state().ui.backup_get_bag_size);
    if (backup == nullptr) return 0;
    if (g_bag_size_depth > 0) return backup(bag);
    BagSizeDepthScope depth;
    // 非 EB 显示刷新窗口（尤其是 core.item 的原版袋容量查询）必须看到真实容量。
    if (g_suppressed || g_bag_size_projection_depth == 0) return backup(bag);

    int capacity = 0;
    if (projected_capacity_for_bag(bag, &capacity)) return capacity;
    return backup(bag);
}

void extension_bag_display_begin_pending(qol::contracts::QolUiRootKind kind, int bag,
                                         int window_bag) {
    g_pending.active = true;
    g_pending.kind = kind;
    g_pending.bag = bag;
    g_pending.window_bag = window_bag;
}

void extension_bag_display_commit() { g_pending = {}; }

void extension_bag_display_cancel() { g_pending = {}; }

BagSizeProjectionScope::BagSizeProjectionScope() {
    ++g_bag_size_projection_depth;
    entered_ = true;
}

BagSizeProjectionScope::~BagSizeProjectionScope() {
    if (entered_) --g_bag_size_projection_depth;
}

DisplaySubstitutionSuppressScope::DisplaySubstitutionSuppressScope() {
    previous_ = g_suppressed;
    g_suppressed = true;
}

DisplaySubstitutionSuppressScope::~DisplaySubstitutionSuppressScope() {
    g_suppressed = previous_;
}

}  // namespace qol::features::extension_bag
