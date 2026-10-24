#include "plugins/extension_bag/runtime/native_tab_layout.h"

#include "plugins/extension_bag/extension_bag_context.h"
#include "plugins/extension_bag/extension_bag_geometry.h"
#include "plugins/extension_bag/runtime/equip_tab_layout.h"
#include "plugins/extension_bag/runtime/logging.h"
#include "plugins/extension_bag/runtime/state.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <mutex>

namespace {
struct C18LayoutDiagnosticKey {
    void* root = nullptr;
    uint64_t generation = 0;
};
std::array<C18LayoutDiagnosticKey, 32> g_c18_layout_diagnostic_keys{};
size_t g_c18_layout_diagnostic_next = 0;
std::mutex g_c18_layout_diagnostic_mutex;

struct C18GeometryPlanCache {
    bool valid = false;
    void* root = nullptr;
    uint64_t generation = 0;
    size_t child_count = 0;
    int64_t parent_x = 0;
    int64_t parent_y = 0;
    int64_t screen_width = 0;
    int64_t screen_height = 0;
    std::array<qol::extension_bag::NativeTabBounds,
               qol::extension_bag::kMaxEquipBagChildren> children{};
    qol::extension_bag::C18ReflowPlan plan{};
};
C18GeometryPlanCache g_c18_geometry_plan_cache{};

struct C18AppliedReflow {
    bool active = false;
    void* root = nullptr;
    uint64_t generation = 0;
    int64_t delta = 0;
    std::array<uint8_t, qol::extension_bag::kMaxEquipBagChildren> changed{};
    std::array<qol::contracts::UiRect, qol::extension_bag::kMaxEquipBagChildren> original_rects{};
};
C18AppliedReflow g_c18_applied_reflow{};

bool same_c18_geometry(const C18GeometryPlanCache& cache, void* root, uint64_t generation,
                       size_t count, int64_t parent_x, int64_t parent_y,
                       int64_t screen_width, int64_t screen_height,
                       const std::array<qol::extension_bag::NativeTabBounds,
                                        qol::extension_bag::kMaxEquipBagChildren>& children) {
    if (!cache.valid || cache.root != root || cache.generation != generation ||
        cache.child_count != count || cache.parent_x != parent_x || cache.parent_y != parent_y ||
        cache.screen_width != screen_width || cache.screen_height != screen_height) return false;
    for (size_t i = 0; i < count; ++i) {
        const auto& a = cache.children[i];
        const auto& b = children[i];
        if (a.x != b.x || a.y != b.y || a.width != b.width || a.height != b.height ||
            a.local_x != b.local_x || a.local_y != b.local_y ||
            a.module_control != b.module_control || a.native_tab != b.native_tab) return false;
    }
    return true;
}

QolResult c18_layout_not_ready(void* parent, const char* reason, int detail,
                               int64_t inset_width = -1, int64_t inset_height = -1,
                               int64_t screen_width = 0, int64_t screen_height = 0) {
    const uint64_t generation = runtime_state().ui.inventory_generation;
    bool should_log = false;
    {
        std::lock_guard<std::mutex> lock(g_c18_layout_diagnostic_mutex);
        for (const auto& key : g_c18_layout_diagnostic_keys) {
            if (key.root == parent && key.generation == generation) return
                qol::contracts::error(kQolErrNotReady, reason, detail);
        }
        g_c18_layout_diagnostic_keys[g_c18_layout_diagnostic_next] = {parent, generation};
        g_c18_layout_diagnostic_next = (g_c18_layout_diagnostic_next + 1) %
            g_c18_layout_diagnostic_keys.size();
        should_log = true;
    }
    if (should_log) {
        int child_count = 0;
        const bool count_ok = extension_bag_ui_child_count(parent, &child_count);
        VIRTBAG_LOG("C18 tab layout fail-closed root=%p generation=%llu child_count=%d count_ok=%d "
                    "insets=%lldx%lld screen=%lldx%lld topology=reject reason=%s detail=%d",
                    parent, static_cast<unsigned long long>(generation), child_count,
                    count_ok ? 1 : 0, static_cast<long long>(inset_width),
                    static_cast<long long>(inset_height), static_cast<long long>(screen_width),
                    static_cast<long long>(screen_height), reason, detail);
        const int limit = count_ok && child_count > 0
            ? (child_count < static_cast<int>(qol::extension_bag::kMaxEquipBagChildren)
                ? child_count : static_cast<int>(qol::extension_bag::kMaxEquipBagChildren))
            : 0;
        for (int index = 0; index < limit; ++index) {
            void* child = extension_bag_ui_child_at(parent, index);
            const bool parent_valid = child != nullptr && extension_bag_ui_parent(child) == parent;
            const int user_type = child != nullptr ? extension_bag_ui_user_type(child) : -1;
            bool module_anchor = false;
            if (child != nullptr && parent_valid) {
                for (const auto& anchor : runtime_state().ui.extension_tab_anchors) {
                    if (anchor.mounted != 0 && anchor.handle == child &&
                        extension_bag_ui_parent(anchor.handle) == parent) {
                        module_anchor = true;
                        break;
                    }
                }
            }
            qol::contracts::UiRect rect{};
            const bool rect_ok = child != nullptr && extension_bag_ui_rect(child, &rect);
            int64_t origin_x = 0, origin_y = 0;
            const bool origin_ok = child != nullptr &&
                extension_bag_ui_control_origin(child, &origin_x, &origin_y);
            VIRTBAG_LOG("C18 tab child index=%d handle=%p type2=%d user_type=%d parent_valid=%d "
                        "module_anchor=%d rect_ok=%d rect=%lld,%lld,%lld,%lld origin_ok=%d "
                        "origin=%lld,%lld",
                        index, child, user_type == 2 ? 1 : 0, user_type,
                        parent_valid ? 1 : 0, module_anchor ? 1 : 0, rect_ok ? 1 : 0,
                        static_cast<long long>(rect.x), static_cast<long long>(rect.y),
                        static_cast<long long>(rect.w), static_cast<long long>(rect.h),
                        origin_ok ? 1 : 0, static_cast<long long>(origin_x),
                        static_cast<long long>(origin_y));
        }
    }
    return qol::contracts::error(kQolErrNotReady, reason, detail);
}

QolResult resolve_c18_layout(void* parent,
    std::array<qol::extension_bag::NativeTabBounds,
               qol::extension_bag::kMaxEquipBagChildren>* out_children,
    int* out_child_count, int64_t* out_parent_x, int64_t* out_parent_y,
    qol::extension_bag::C18ReflowPlan* out_plan) {
    if (out_children == nullptr || out_child_count == nullptr || out_parent_x == nullptr ||
        out_parent_y == nullptr || out_plan == nullptr) {
        return qol::contracts::error(kQolErrInvalidArgument, "C18 layout output", 1);
    }
    int child_count = 0;
    if (!extension_bag_ui_child_count(parent, &child_count) || child_count < 6 ||
        child_count > static_cast<int>(qol::extension_bag::kMaxEquipBagChildren)) {
        return c18_layout_not_ready(parent, "native controls incomplete", 15);
    }
    out_children->fill({});
    for (int child_index = 0; child_index < child_count; ++child_index) {
        auto& candidate = (*out_children)[static_cast<size_t>(child_index)];
        candidate.native_tab = false;
        void* child = extension_bag_ui_child_at(parent, child_index);
        if (child == nullptr || extension_bag_ui_parent(child) != parent) {
            return c18_layout_not_ready(parent, "native controls incomplete", 15);
        }
        bool module_control = false;
        for (const auto& anchor : runtime_state().ui.extension_tab_anchors) {
            if (anchor.mounted != 0 && anchor.handle == child &&
                extension_bag_ui_parent(anchor.handle) == parent) {
                module_control = true;
                break;
            }
        }
        if (module_control) {
            candidate.module_control = true;
            continue;
        }
        if (extension_bag_ui_user_type(child) != 2) continue;
        qol::contracts::UiRect rect{};
        if (!extension_bag_ui_rect(child, &rect)) {
            return c18_layout_not_ready(parent, "native control bounds unavailable", 17);
        }
        candidate.native_tab = true;
        if (rect.w <= 0 || rect.h <= 0) continue;
        int64_t abs_x = 0, abs_y = 0;
        if (!extension_bag_ui_control_origin(child, &abs_x, &abs_y)) {
            return c18_layout_not_ready(parent, "native control origin unavailable", 19);
        }
        candidate.x = abs_x;
        candidate.y = abs_y;
        candidate.width = rect.w;
        candidate.height = rect.h;
        candidate.local_x = rect.x;
        candidate.local_y = rect.y;
    }
    if (!extension_bag_ui_control_origin(parent, out_parent_x, out_parent_y)) {
        return c18_layout_not_ready(parent, "bag origin unavailable", 20);
    }
    const auto* ui = runtime_state().host.sdk != nullptr ? runtime_state().host.sdk->ui : nullptr;
    constexpr size_t inset_height_end = offsetof(qol::contracts::QolUiAdapter, screen_inset_height) +
        sizeof(((qol::contracts::QolUiAdapter*)nullptr)->screen_inset_height);
    int32_t inset_width = -1, inset_height = -1;
    if (ui == nullptr || ui->size < inset_height_end || ui->screen_inset_width == nullptr ||
        ui->screen_inset_height == nullptr ||
        !qol::contracts::succeeded(ui->screen_inset_width(&inset_width)) ||
        !qol::contracts::succeeded(ui->screen_inset_height(&inset_height)) ||
        inset_width < 0 || inset_height < 0) {
        return c18_layout_not_ready(parent, "screen bounds unavailable", 21,
                                    inset_width, inset_height);
    }
    const int64_t screen_width = 960 + static_cast<int64_t>(inset_width) * 2;
    const int64_t screen_height = 640 + static_cast<int64_t>(inset_height) * 2;
    const uint64_t generation = runtime_state().ui.inventory_generation;
    if (same_c18_geometry(g_c18_geometry_plan_cache, parent, generation,
            static_cast<size_t>(child_count), *out_parent_x, *out_parent_y,
            screen_width, screen_height, *out_children)) {
        *out_plan = g_c18_geometry_plan_cache.plan;
    } else {
        if (!qol::extension_bag::c18_extension_tab_layout(*out_children,
                static_cast<size_t>(child_count), *out_parent_x, *out_parent_y,
                screen_width, screen_height, out_plan)) {
            g_c18_geometry_plan_cache.valid = false;
            return c18_layout_not_ready(parent, "native layout ambiguous or out of bounds", 18,
                                        inset_width, inset_height, screen_width, screen_height);
        }
        g_c18_geometry_plan_cache.valid = true;
        g_c18_geometry_plan_cache.root = parent;
        g_c18_geometry_plan_cache.generation = generation;
        g_c18_geometry_plan_cache.child_count = static_cast<size_t>(child_count);
        g_c18_geometry_plan_cache.parent_x = *out_parent_x;
        g_c18_geometry_plan_cache.parent_y = *out_parent_y;
        g_c18_geometry_plan_cache.screen_width = screen_width;
        g_c18_geometry_plan_cache.screen_height = screen_height;
        g_c18_geometry_plan_cache.children = *out_children;
        g_c18_geometry_plan_cache.plan = *out_plan;
    }
    *out_child_count = child_count;
    return qol::contracts::ok();
}

bool c18_shifted_local_x(int64_t original_x, int64_t delta, int64_t* out_x) {
    if (out_x == nullptr || delta < 0 ||
        original_x > std::numeric_limits<int64_t>::max() - delta) return false;
    *out_x = original_x + delta;
    return true;
}


}  // namespace

void prepare_c18_native_bag_layout_locked_impl() {
    const auto* game = runtime_state().host.game;
    if (game == nullptr || game->target.has_capability == nullptr ||
        game->target.has_capability(
            qol::contracts::kQolTargetCapabilityC18NativeBagLayoutCandidate) == 0) return;
    void* parent = extension_bag_ui_root(qol::contracts::QolUiRootKind::kEquipBagContainer);
    if (parent == nullptr) return;
    const uint64_t generation = runtime_state().ui.inventory_generation;
    if (g_c18_applied_reflow.active &&
        (g_c18_applied_reflow.root != parent || g_c18_applied_reflow.generation != generation)) {
        // Root/generation changes invalidate geometry; never reuse old handles.
        g_c18_applied_reflow = {};
    }
    const bool should_reflow = runtime_state().enabled.load() &&
        extension_bag_session_admitted_locked() && !runtime_state().store.view_installed;
    if (!should_reflow) {
        if (!g_c18_applied_reflow.active) return;
        int child_count = 0;
        if (!extension_bag_ui_child_count(parent, &child_count)) return;
        std::array<int32_t, qol::extension_bag::kMaxEquipBagChildren> restored{};
        size_t restored_count = 0;
        for (size_t i = 0; i < g_c18_applied_reflow.changed.size(); ++i) {
            if (g_c18_applied_reflow.changed[i] == 0) continue;
            if (i >= static_cast<size_t>(child_count)) return;
            void* control = extension_bag_ui_child_at(parent, static_cast<int>(i));
            qol::contracts::UiRect current{};
            const auto& original = g_c18_applied_reflow.original_rects[i];
            int64_t shifted_x = 0;
            if (control == nullptr || extension_bag_ui_parent(control) != parent ||
                extension_bag_ui_user_type(control) != 2 ||
                !extension_bag_ui_rect(control, &current) ||
                !c18_shifted_local_x(original.x, g_c18_applied_reflow.delta, &shifted_x) ||
                current.x != shifted_x ||
                current.y != original.y || current.w != original.w || current.h != original.h ||
                !extension_bag_ui_set_rect(control, original)) {
                for (size_t n = restored_count; n > 0; --n) {
                    const int index = restored[n - 1];
                    void* rollback = extension_bag_ui_child_at(parent, index);
                    const auto& saved = g_c18_applied_reflow.original_rects[static_cast<size_t>(index)];
                    int64_t shifted_x = 0;
                    if (rollback != nullptr && extension_bag_ui_parent(rollback) == parent) {
                        if (c18_shifted_local_x(saved.x, g_c18_applied_reflow.delta, &shifted_x)) {
                            (void)extension_bag_ui_set_rect(rollback,
                                {shifted_x, saved.y, saved.w, saved.h});
                        }
                    }
                }
                VIRTBAG_LOG("C18 native reflow restore deferred root=%p generation=%llu child=%zu",
                            parent, static_cast<unsigned long long>(generation), i);
                return;
            }
            restored[restored_count++] = static_cast<int32_t>(i);
        }
        VIRTBAG_LOG("C18 native reflow restored root=%p generation=%llu",
                    parent, static_cast<unsigned long long>(generation));
        g_c18_applied_reflow = {};
        return;
    }
    std::array<qol::extension_bag::NativeTabBounds,
               qol::extension_bag::kMaxEquipBagChildren> children{};
    int child_count = 0;
    int64_t parent_x = 0, parent_y = 0;
    qol::extension_bag::C18ReflowPlan plan{};
    if (!qol::contracts::succeeded(resolve_c18_layout(parent, &children, &child_count,
            &parent_x, &parent_y, &plan)) || plan.vertical_delta == 0) return;
    if (g_c18_applied_reflow.active) {
        VIRTBAG_LOG("C18 native reflow unexpected renewed collision root=%p generation=%llu",
                    parent, static_cast<unsigned long long>(generation));
        return;
    }
    std::array<int32_t, qol::extension_bag::kMaxEquipBagChildren> changed{};
    size_t changed_count = 0;
    for (int i = 0; i < child_count; ++i) {
        if (plan.move_vertical[static_cast<size_t>(i)] == 0) continue;
        void* control = extension_bag_ui_child_at(parent, i);
        const auto& candidate = children[static_cast<size_t>(i)];
        qol::contracts::UiRect current_rect{};
        int64_t shifted_x = 0;
        if (control == nullptr || extension_bag_ui_parent(control) != parent ||
            extension_bag_ui_user_type(control) != 2 ||
            !extension_bag_ui_rect(control, &current_rect) ||
            current_rect.x != candidate.local_x || current_rect.y != candidate.local_y ||
            current_rect.w != candidate.width || current_rect.h != candidate.height ||
            !c18_shifted_local_x(candidate.local_x, plan.vertical_delta, &shifted_x) ||
            !extension_bag_ui_set_rect(control,
                {shifted_x, candidate.local_y,
                 candidate.width, candidate.height})) {
            for (size_t n = changed_count; n > 0; --n) {
                const int index = changed[n - 1];
                void* rollback = extension_bag_ui_child_at(parent, index);
                const auto& original = children[static_cast<size_t>(index)];
                if (rollback != nullptr && extension_bag_ui_parent(rollback) == parent) {
                    (void)extension_bag_ui_set_rect(rollback,
                        {original.local_x, original.local_y, original.width, original.height});
                }
            }
            VIRTBAG_LOG("C18 native reflow aborted root=%p generation=%llu failed_child=%d",
                        parent, static_cast<unsigned long long>(runtime_state().ui.inventory_generation), i);
            return;
        }
        changed[changed_count++] = i;
    }
    VIRTBAG_LOG("C18 native reflow applied root=%p generation=%llu moved=%zu delta=%lld",
                parent, static_cast<unsigned long long>(runtime_state().ui.inventory_generation),
                changed_count, static_cast<long long>(plan.vertical_delta));
    g_c18_applied_reflow.active = changed_count != 0;
    g_c18_applied_reflow.root = parent;
    g_c18_applied_reflow.generation = generation;
    g_c18_applied_reflow.delta = plan.vertical_delta;
    for (size_t n = 0; n < changed_count; ++n) {
        const size_t index = static_cast<size_t>(changed[n]);
        g_c18_applied_reflow.changed[index] = 1;
        const auto& candidate = children[index];
        g_c18_applied_reflow.original_rects[index] = {
            candidate.local_x, candidate.local_y, candidate.width, candidate.height};
    }
}

QolResult c18_native_tab_anchor_layout_locked(void* parent, int index,
                                               qol::contracts::UiRect* out_rect) {
    std::array<qol::extension_bag::NativeTabBounds,
               qol::extension_bag::kMaxEquipBagChildren> native_candidates{};
    int child_count = 0;
    int64_t parent_x = 0, parent_y = 0;
    qol::extension_bag::C18ReflowPlan plan{};
    const QolResult resolved = resolve_c18_layout(parent, &native_candidates, &child_count,
                                                   &parent_x, &parent_y, &plan);
    if (!qol::contracts::succeeded(resolved)) return resolved;
    *out_rect = {plan.module_x, plan.module_y[static_cast<size_t>(index)], 57, 57};
    return qol::contracts::ok();
}

bool c18_native_bag_hit_locked(int64_t x, int64_t y, int* out_native_bag) {
    if (out_native_bag == nullptr) return false;
    const auto* game = runtime_state().host.game;
    if (game == nullptr || game->target.has_capability == nullptr ||
        game->target.has_capability(
            qol::contracts::kQolTargetCapabilityC18NativeBagLayoutCandidate) == 0) return false;
    void* parent = extension_bag_ui_root(qol::contracts::QolUiRootKind::kEquipBagContainer);
    if (parent == nullptr) return false;
    int child_count = 0;
    if (!extension_bag_ui_child_count(parent, &child_count)) return false;
    if (child_count > static_cast<int>(qol::extension_bag::kMaxEquipBagChildren)) {
        child_count = static_cast<int>(qol::extension_bag::kMaxEquipBagChildren);
    }
    // 原生 UIEquip_GetBagSlotIndex 就是“控件在袋容器里的子序号”，袋号与子序号同源。
    // 排除模块页签（扩展页签也挂在同一容器），歧义命中一律拒绝。
    int hit_count = 0;
    int found = -1;
    for (int i = 0; i < child_count; ++i) {
        void* control = extension_bag_ui_child_at(parent, i);
        if (control == nullptr || extension_bag_ui_parent(control) != parent ||
            extension_bag_ui_user_type(control) != 2) continue;
        bool module_tab = false;
        for (size_t slot = 0; slot < runtime_state().ui.extension_tab_buttons.size(); ++slot) {
            if (runtime_state().ui.extension_tab_buttons[slot] == control) {
                module_tab = true;
                break;
            }
        }
        if (module_tab) continue;
        qol::contracts::UiRect rect{};
        if (!extension_bag_ui_rect(control, &rect) || rect.w <= 0 || rect.h <= 0) continue;
        if (!extension_bag_ui_hit_test(control, x, y, rect)) continue;
        found = i;
        ++hit_count;
    }
    if (hit_count != 1 || found < 0) return false;
    *out_native_bag = found;
    return true;
}
