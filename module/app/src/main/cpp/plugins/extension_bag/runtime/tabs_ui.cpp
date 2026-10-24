#include "game_ui_virtbag.h"

#include "contracts/game_host.h"
#include "contracts/host_sdk.h"
#include "contracts/ui.h"
#include "contracts/ownership.h"
#include "contracts/inventory_refresh.h"
#include "contracts/item_facts.h"
#include "plugins/extension_bag/model/equip_rules.h"
#include "plugins/extension_bag/extension_bag_port.h"
#include "plugins/extension_bag/assembly/move_merge.h"
#include "plugins/extension_bag/model/ownership_ledger.h"
#include "plugins/extension_bag/model/item_handle_custody.h"
#include "plugins/extension_bag/model/sale_transaction.h"
#include "plugins/extension_bag/model/draw_state_lock.h"
#include "plugins/extension_bag/model/main_menu_cleanup_edge.h"
#include "plugins/extension_bag/model/virtual_bag_state.h"
#include "contracts/module_save_port.h"
#include "contracts/popup_enter_alias.h"
#include "plugins/extension_bag/extension_bag_context.h"
#include "plugins/extension_bag/extension_bag_geometry.h"
#include "plugins/extension_bag/runtime/logging.h"
#include "plugins/extension_bag/plugin.h"
#include "plugins/extension_bag/ui/store_sell_rules.h"
#include "plugins/extension_bag/extension_bag_internal.h"
#include "plugins/extension_bag/runtime/entry_guard.h"
#include "plugins/extension_bag/runtime/equip_tab_layout.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <functional>
#include <mutex>
#include <thread>
#include <unistd.h>

#define handle_extension_tab_click_locked handle_extension_tab_click_locked_internal
#define delete_extension_tab_buttons_locked delete_extension_tab_buttons_locked_internal
#define disable_extension_tab_buttons_locked disable_extension_tab_buttons_locked_internal
#define install_extension_tab_buttons_locked install_extension_tab_buttons_locked_internal
#define extension_tab_buttons_complete_locked extension_tab_buttons_complete_locked_internal
#define commit_pending_extension_tab_locked commit_pending_extension_tab_locked_internal
#define reset_extension_desc_unequip_hook_locked reset_extension_desc_unequip_hook_locked_internal
#define extension_tab_hit extension_tab_hit_internal

namespace {
constexpr size_t kPopupStateSize = 0x40;
constexpr int kPopupStateCount = 27;
constexpr int64_t kCellStepY = 0x46;
constexpr size_t kInventorySlotStride = 16;
// Extension tab ControlObject rects are relative to the item-list root, whose
// absolute origin is (kGridX, kGridY). Their visuals keep the existing overlay
// positions so this phase changes input ownership without moving the UI.
constexpr int64_t kExtensionTabX = kCellX - kGridX;
constexpr int64_t kExtensionTabY = kCellY - kGridY;
constexpr int64_t kExtensionTabWidth = 0x70;
constexpr int64_t kExtensionTabHeight = 0x30;
void queue_extension_tab_click_locked(int extension_bag);
bool is_backpack_category(int category) {
    // 这是 State::types 使用的 BagType 判定：1..4 派生容量 4/8/12/16，
    // 只适用于已归一化的扩展袋类型。它不是原生物品类别判定；原生物品
    // 详情必须使用 category_is_extension_backpack 的 ITEMCLASSBASE +2 规则。
    return category >= 1 && category <= 4;
}

bool extension_tab_control_is_current_locked(int index, void* control);
bool extension_tab_buttons_complete_locked();
bool extension_tab_buttons_have_controls_locked();

uint64_t extension_tab_item_proc(void* ctrl, uint64_t event, void* x2, void* param) {
    qol::extension_bag::EntryGuard guard(
        "tab_proc", qol::extension_bag::ReentryPolicy::kRejectWrite);
    if (!guard.entered()) return 0;
    const uint64_t observation_token = event == 0x10
        ? 0 : virtual_bag_observe_item_proc_pre(ctrl, event, x2, param);
    const auto finish = [observation_token, ctrl, event, x2, param](uint64_t result) {
        virtual_bag_observe_item_proc_post(observation_token, ctrl, event, x2, param, result);
        return result;
    };
    if (event == 0x02) {
        uint64_t result = 1;
        {
            // 原生事件分发可能在模块已持锁的调用栈内回调本 proc；拿不到锁时
            // 不能阻塞，直接放行让原版处理，避免自死锁。
            std::unique_lock<std::mutex> lock(runtime_state().mtx, std::try_to_lock);
            if (!lock.owns_lock()) {
                VIRTBAG_LOG("extension tab proc busy event=0x%llx",
                            static_cast<unsigned long long>(event));
                return finish(0);
            }
            if (runtime_state().enabled.load() && extension_bag_session_admitted_locked()) {
                const int index = extension_tab_index(ctrl);
                if (index >= 0) queue_extension_tab_click_locked(index);
            }
        }
        // Observation post reacquires this mutex, so finish only after leaving the scope.
        return finish(result);  // 松开确认已处理（原版袋标签点击切袋同样返回 1，b8c60）
    }
    // 0x04 = 拖动 drop 到袋控件。无论原版源还是扩展源，只要目标袋未装备且
    // 源类别为背包物品（category 1..4）就走装备；其他情况走移动。装备路径
    // 返回 0，让 TouchHandle 按原版语义复位 moving。
    if (event == 0x04) {
        uint64_t result = 0;
        {
            // 同 0x02：drop 也可能在模块持锁的原生调用栈内到达。
            std::unique_lock<std::mutex> lock(runtime_state().mtx, std::try_to_lock);
            if (!lock.owns_lock()) {
                VIRTBAG_LOG("extension tab proc busy event=0x%llx",
                            static_cast<unsigned long long>(event));
                return finish(0);
            }
            if (!runtime_state().enabled.load() || !extension_bag_session_admitted_locked()) {
                result = 1;
            } else {
                const int index = extension_tab_index(ctrl);
                if (index >= 0 && extension_tab_control_is_current_locked(index, ctrl) &&
                    runtime_state().ui.extension_drag_session.phase != virtual_bag::DragPhase::kIdle) {
                    const int logical_bag = virtual_bag::extension_logical_bag(index);
                    const bool handled = logical_bag >= 0 &&
                                         route_projected_session_to_tab_locked(lock, logical_bag);
                    if (logical_bag < 0) {
                        VIRTBAG_LOG("cross tab reject reason=target_tab_index_invalid index=%d", index);
                    }
                    result = handled ? 1 : 0;
                } else {
                    void* moving = touch_moving_item_control_locked();
                    // 原版不可堆叠装备在 drop 事件到达时可能已经清掉
                    // ITEM_CTRL_MOVING_FLAG，但事件参数仍保留真实 source control。
                    // 优先使用 TouchHandle 状态，只有状态已清空时才回退到事件源，避免
                    // 放宽普通点击/无效控件的处理范围。
                    if (moving == nullptr && param != nullptr) {
                        moving = *reinterpret_cast<void**>(reinterpret_cast<uint8_t*>(param) + 8);
                    }
                    if (index >= 0 && moving != nullptr) {
                        void* data = extension_bag_ui_data(moving);
                        void* item = data != nullptr ? *reinterpret_cast<void**>(data) : nullptr;
                        int module_bag = -1;
                        int module_slot = -1;
                        const bool extension_source =
                            item != nullptr && module_slot_of_item_locked(item, &module_bag, &module_slot);
                        const int category = extension_source
                            ? runtime_state().objects.bag.items[module_bag][module_slot].category
                            : original_item_category_locked(item);
                        const bool backpack_item = is_backpack_category(category);
                        if (runtime_state().objects.bag.types[index] == 0 && backpack_item) {
                            // 背包类物品进入空扩展标签 = 装备为该袋的袋对象；来源可以是
                            // 原版背包，也可以是另一个扩展袋。两者都由同一事务处理。
                            try_equip_on_extension_tab_drop_locked(lock, index, moving);
                        } else if (extension_source) {
                            // 目标袋已有背包对象，或来源是装备/其他物品：一律按普通
                            // 扩展袋间移动处理，不能把“背包槽装备”误当人物装备。
                            const bool moved = move_extension_to_extension_locked(
                                module_bag, module_slot, index, -1);
                            if (moved) {
                                persist_state_locked(false);
                                refresh_projected_module_view_after_move_locked();
                            }
                            VIRTBAG_LOG("extension tab drop extension move result=%d index=%d category=%d",
                                        moved ? 1 : 0, index, category);
                        } else {
                            const bool moved = move_original_to_extension_locked(lock, index, moving);
                            VIRTBAG_LOG("extension tab drop original move result=%d index=%d category=%d equipped=%d",
                                        moved ? 1 : 0, index, category,
                                        runtime_state().objects.bag.types[index] != 0 ? 1 : 0);
                        }
                    }
                }
            }
        }
        return finish(result);
    }
    // Native MoveOn queries this proc while traversing its current control tree.
    // Only claim eligibility when an item is already being dragged, so the tab is
    // never registered as a drag source on its own press; and never block on the
    // runtime mutex from that native callback stack.
    if (event == 0x10) {
        std::unique_lock<std::mutex> lock(runtime_state().mtx, std::try_to_lock);
        if (!lock.owns_lock()) return finish(0);
        return finish(touch_moving_item_control_locked() != nullptr ? 1 : 0);
    }
    // 其余未处理事件返回 0；原版控件对未处理事件同样返回 0。
    return finish(0);
}

// 扩展标签 = ControlItem（与原版袋标签同类）：挂袋容器（0x3049e0+0x50），
// SetUserType(2) + SetControlProc（自定义，0x02 松开确认切换/详情 toggle）。
// data[0] = 按袋 types 物化的真实背包物品对象（TouchHandle/详情链合法）。
// 手工父链累加（自上而下）：计算控件绝对位置。CO_RECT 为 i64 相对父值。
void delete_extension_tab_buttons_locked() {
    // 句柄存活由锚定层证明：只删除仍能证明属于本模块的控件；失效句柄只清记录。
    const auto* components = extension_bag_ui_components();
    for (int index = 0; index < virtual_bag::kBagCount; ++index) {
        auto& state = runtime_state().ui.extension_tab_anchors[static_cast<size_t>(index)];
        void* handle = nullptr;
        uint8_t live = 0;
        if (components != nullptr && components->anchored_control_detach != nullptr) {
            (void)components->anchored_control_detach(&state, &handle, &live);
        } else {
            state = {};
        }
        if (live != 0 && handle != nullptr && !extension_bag_ui_delete_control(handle)) {
            VIRTBAG_LOG("extension tab delete failed control=%p", handle);
        }
        runtime_state().ui.extension_tab_buttons[static_cast<size_t>(index)] = nullptr;
    }
}

void invalidate_extension_tab_buttons_locked() {
    cancel_projected_drag_session_locked("extension-tab-root-change");
    ++runtime_state().ui.inventory_generation;
    runtime_state().ui.pending_extension_tab = -1;
    runtime_state().ui.pending_extension_tab_generation = 0;
    runtime_state().ui.extension_tab_buttons.fill(nullptr);
    runtime_state().ui.extension_tab_anchors = {};
    runtime_state().ui.extension_tab_root = nullptr;
    runtime_state().ui.extension_tab_generation = 0;
}

// ---- 页签锚定（ui_components minor 21）----
// 页签挂在游戏袋容器上，游戏在切页/关闭面板时销毁整棵子树。句柄存活/身份校验/
// 失效重建统一交给锚定层：本文件只提供几何（layout）与建控件（create）回调。

struct EquipTabAnchorContext {
    int index = 0;
};
std::array<EquipTabAnchorContext, virtual_bag::kBagCount> g_equip_tab_anchor_ctx{};

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

QolResult equip_tab_anchor_layout(void* parent, qol::contracts::UiRect* out_rect, void* user) {
    if (parent == nullptr || out_rect == nullptr || user == nullptr) {
        return qol::contracts::error(kQolErrInvalidArgument, "tab layout", 10);
    }
    const auto* ctx = static_cast<const EquipTabAnchorContext*>(user);
    if (!virtual_bag::valid_index(ctx->index)) {
        return qol::contracts::error(kQolErrInvalidArgument, "tab index", 9);
    }
    const auto* game = runtime_state().host.game;
    const bool c18_grid = game != nullptr && game->target.has_capability != nullptr &&
        game->target.has_capability(
            qol::contracts::kQolTargetCapabilityC18NativeBagLayoutCandidate) != 0;
    if (c18_grid) {
        std::array<qol::extension_bag::NativeTabBounds,
                   qol::extension_bag::kMaxEquipBagChildren> native_candidates{};
        int child_count = 0;
        int64_t parent_x = 0, parent_y = 0;
        qol::extension_bag::C18ReflowPlan plan{};
        const QolResult resolved = resolve_c18_layout(parent, &native_candidates, &child_count,
                                                       &parent_x, &parent_y, &plan);
        if (!qol::contracts::succeeded(resolved)) return resolved;
        *out_rect = {plan.module_x, plan.module_y[static_cast<size_t>(ctx->index)], 57, 57};
        return qol::contracts::ok();
    }
    int child_count = 0;
    if (!extension_bag_ui_child_count(parent, &child_count) || child_count < 6) {
        return qol::contracts::error(kQolErrNotReady, "bag container not ready", 24);
    }
    void* bag0 = valid_child_locked(parent, 0);
    qol::contracts::UiRect bag0_rect{};
    if (bag0 == nullptr || !extension_bag_ui_rect(bag0, &bag0_rect) || bag0_rect.w <= 0) {
        return qol::contracts::error(kQolErrNotReady, "bag0 not ready", 15);
    }
    int64_t bag0_abs_x = 0, bag0_abs_y = 0, mount_abs_x = 0, mount_abs_y = 0;
    extension_bag_control_abs_pos(bag0, &bag0_abs_x, &bag0_abs_y);
    extension_bag_control_abs_pos(parent, &mount_abs_x, &mount_abs_y);
    *out_rect = {bag0_abs_x + 68 - mount_abs_x,
        bag0_abs_y + 2 - mount_abs_y + ctx->index * 70, 57, 57};
    return qol::contracts::ok();
}

QolResult equip_tab_anchor_create(void* parent, qol::contracts::UiRect rect, void* user,
                                  void** out_control) {
    if (parent == nullptr || user == nullptr || out_control == nullptr) {
        return qol::contracts::error(kQolErrInvalidArgument, "tab create", 10);
    }
    *out_control = nullptr;
    const auto* ctx = static_cast<const EquipTabAnchorContext*>(user);
    if (!virtual_bag::valid_index(ctx->index)) {
        return qol::contracts::error(kQolErrInvalidArgument, "tab index", 9);
    }
    // ControlItem_Create 语义组装：Add(parent, 0, 0, type=3, proc) + SetControlProc + SetUserType(2)。
    // 第 5 参 = proc，经 CreateControlInfo 写入 +0x90（CO_PROC，TouchHandle 统一事件分发）。
    void* ctrl = nullptr;
    if (!extension_bag_ui_create_control(parent, &ctrl) || ctrl == nullptr) {
        VIRTBAG_LOG("extension tab create failed index=%d", ctx->index);
        return qol::contracts::error(kQolErrNotReady, "tab create", 10);
    }
    const auto fail = [ctrl](const char* reason, int index) {
        (void)extension_bag_ui_delete_control(ctrl);
        VIRTBAG_LOG("extension tab setup failed reason=%s index=%d control=%p", reason, index, ctrl);
        return qol::contracts::error(kQolErrNotReady, "tab setup", 9);
    };
    if (!extension_bag_ui_set_control_proc(ctrl, &extension_tab_item_proc)) {
        return fail("proc", ctx->index);
    }
    if (!extension_bag_ui_set_control_user_type(ctrl, 2)) return fail("user-type", ctx->index);
    if (!extension_bag_ui_unuse_control_event_move(ctrl)) return fail("event-move", ctx->index);
    if (!extension_bag_ui_use_control_event_on(ctrl)) return fail("event-on", ctx->index);
    if (!extension_bag_ui_set_rect(ctrl, rect)) return fail("rect", ctx->index);
    // data：16B（+0x0=item、+0x8..0xb=标志），装箱背包物品对象
    void* data = nullptr;
    if (!extension_bag_ui_allocate_control_data(0x10, &data) || data == nullptr) {
        return fail("data-allocate", ctx->index);
    }
    std::memset(data, 0, 0x10);
    *reinterpret_cast<void**>(data) = runtime_state().ui.tab_bag_items[ctx->index];
    if (!extension_bag_ui_set_control_data(ctrl, data)) return fail("data-set", ctx->index);
    *out_control = ctrl;
    return qol::contracts::ok();
}

// 每帧刷新：解析活袋容器、证明/重建 5 个页签，并把句柄同步到运行视图数组。
bool refresh_extension_tab_anchors_locked(const qol::contracts::QolUiComponents* components,
                                          void* parent, bool* out_created_any) {
    if (out_created_any != nullptr) *out_created_any = false;
    if (components == nullptr || components->anchored_control_ensure == nullptr) return false;
    bool all = true;
    for (int index = 0; index < virtual_bag::kBagCount; ++index) {
        auto& ctx = g_equip_tab_anchor_ctx[static_cast<size_t>(index)];
        ctx.index = index;
        const qol::contracts::UiAnchoredControlDesc desc{
            static_cast<uint32_t>(qol::contracts::QolUiRootKind::kEquipBagContainer),
            &extension_tab_item_proc, &equip_tab_anchor_layout, &equip_tab_anchor_create, &ctx};
        uint8_t created = 0;
        auto& state = runtime_state().ui.extension_tab_anchors[static_cast<size_t>(index)];
        const QolResult ensured = components->anchored_control_ensure(&state, &desc, &created);
        if (ensured.ok != kQolOk) {
            all = false;
            // NotReady 是面板打开初期的正常态（占位容器/布局未就位），不刷日志。
            if (ensured.ok != kQolErrNotReady) {
                VIRTBAG_LOG("extension tab anchor ensure failed index=%d code=%d root=%p", index,
                            ensured.ok, parent);
            }
        }
        runtime_state().ui.extension_tab_buttons[static_cast<size_t>(index)] =
            state.mounted != 0 ? state.handle : nullptr;
        if (created != 0) {
            if (out_created_any != nullptr) *out_created_any = true;
            VIRTBAG_LOG("extension tab re-anchored index=%d root=%p", index, parent);
        }
    }
    return all;
}

void install_extension_tab_buttons_locked() {
    // 配置门控：扩展背包关闭时不得挂载页签（3 个宿主之一）。启用/禁用均以
    // g_virtual_bag_enabled 为准；调用方不再各自判断。
    if (!runtime_state().enabled.load() || !extension_bag_session_admitted_locked()) return;
    void* bag_container = extension_bag_ui_root(qol::contracts::QolUiRootKind::kEquipBagContainer);
    if (bag_container == nullptr) return;
    // 容器就位守卫：面板打开初期 0x50 槽位指向占位容器（无原版袋标签子控件，
    // rect 未定位）。等原版 6 袋标签就位后再挂载；未就位由 draw 每帧重试。
    int bag_child_count = 0;
    if (!extension_bag_ui_child_count(bag_container, &bag_child_count) || bag_child_count < 6) return;
    const auto* components = extension_bag_ui_components();
    if (components == nullptr) return;
    refresh_tab_bag_items_locked();
    const bool changed = runtime_state().ui.extension_tab_root != bag_container ||
        runtime_state().ui.extension_tab_generation != runtime_state().ui.inventory_generation;
    bool created_any = false;
    const bool complete = refresh_extension_tab_anchors_locked(components, bag_container,
                                                               &created_any);
    runtime_state().ui.extension_tab_root = bag_container;
    runtime_state().ui.extension_tab_generation = runtime_state().ui.inventory_generation;
    if (changed || created_any) {
        VIRTBAG_LOG("extension tabs installed generation=%llu root=%p complete=%d created=%d",
                    static_cast<unsigned long long>(runtime_state().ui.inventory_generation),
                    bag_container, complete ? 1 : 0, created_any ? 1 : 0);
    }
}

void disable_extension_tab_buttons_locked() {
    void* live_root = extension_bag_ui_root(qol::contracts::QolUiRootKind::kEquipBagContainer);
    if (runtime_state().ui.extension_tab_root != nullptr && live_root == runtime_state().ui.extension_tab_root) {
        delete_extension_tab_buttons_locked();
    } else if (runtime_state().ui.extension_tab_root != nullptr) {
        VIRTBAG_LOG("extension tab references invalidated reason=root_not_live saved=%p live=%p",
                    runtime_state().ui.extension_tab_root, live_root);
    }
    // The game owns controls; stale-root references are invalidated, never deleted.
    invalidate_extension_tab_buttons_locked();
}
bool extension_tab_control_is_current_locked(int index, void* control);
void handle_extension_tab_click_impl_locked(int extension_bag,
                                            std::unique_lock<std::mutex>* unlockable_lock);
bool extension_tab_buttons_complete_locked() {
    for (void* button : runtime_state().ui.extension_tab_buttons) {
        if (button == nullptr) return false;
    }
    return true;
}

bool extension_tab_buttons_have_controls_locked() {
    for (void* button : runtime_state().ui.extension_tab_buttons) {
        if (button != nullptr) return true;
    }
    return false;
}

void reset_extension_desc_unequip_hook_locked() {
    // 卸下按钮由函数级 ButtonUnequipExe hook 承担，
    // 仅清 desc 袋记录。
    runtime_state().ui.extension_desc_unequip_bag = -1;
}

void install_extension_desc_unequip_hook_locked(int extension_bag) {
    reset_extension_desc_unequip_hook_locked();
    if (!virtual_bag::valid_index(extension_bag)) return;
    // 扩展袋 desc 打开时记录袋位，供函数级 ButtonUnequipExe hook
    // （virtual_bag_handle_original_bag_unequip）识别扩展袋解除路径。
    runtime_state().ui.extension_desc_unequip_bag = extension_bag;
    VIRTBAG_LOG("extension desc unequip armed bag=%d", extension_bag);
}

// P3：扩展袋切换音效，与原版袋按钮同款（UIEquip_InvenBagControlEventProc b8c34：Play(0x11)）。
void play_extension_switch_sound() {
    if (runtime_state().host.game == nullptr || runtime_state().host.write_port->ui.play_switch_sound == nullptr) return;
    (void)runtime_state().host.write_port->ui.play_switch_sound();
}

void handle_extension_tab_click_locked(int extension_bag) {
    handle_extension_tab_click_impl_locked(extension_bag, nullptr);
}

void handle_pending_extension_tab_click_locked(std::unique_lock<std::mutex>& lock,
                                               int extension_bag) {
    handle_extension_tab_click_impl_locked(extension_bag, &lock);
}

void handle_extension_tab_click_impl_locked(int extension_bag,
                                            std::unique_lock<std::mutex>* unlockable_lock) {
    if (!virtual_bag::valid_index(extension_bag) ||
        !extension_bag_session_admitted_locked()) return;
    if (runtime_state().objects.bag.mode == virtual_bag::Mode::kOriginal) {
        const int original_bag = original_bag_locked();
        if (original_bag == virtual_bag::kOriginalTaskBag) {
            constexpr uint32_t kCapacityMask = (1u << 25) - 1u;
            void* panel_root = extension_bag_ui_root(qol::contracts::QolUiRootKind::kEquipItemRoot);
            const int target_capacity = runtime_state().objects.bag.capacities[extension_bag];
            const bool target_ready = target_capacity > 0 && panel_root != nullptr;
            int projection_host = -1;
            if (target_ready) {
                for (int candidate = 0;
                     candidate < virtual_bag::kOriginalTransactionBagCount; ++candidate) {
                    uint32_t size_word = 0;
                    if (!bag_size_word_locked(candidate, &size_word) ||
                        (size_word & kCapacityMask) == 0) continue;
                    bool controls_ready = true;
                    for (int slot = 0; slot < target_capacity; ++slot) {
                        if (valid_child_locked(panel_root, slot) == nullptr) {
                            controls_ready = false;
                            break;
                        }
                    }
                    if (controls_ready) {
                        projection_host = candidate;
                        break;
                    }
                }
            }
            if (projection_host < 0) {
                VIRTBAG_LOG(
                    "extension tab reject reason=projection_host_unavailable source_original_bag=%d target=%d",
                    original_bag, extension_bag);
                return;
            }
            set_original_bag_locked(projection_host);
            runtime_state().objects.original_current_direct = static_cast<uint8_t>(projection_host);
            runtime_state().objects.original_current_got = static_cast<uint8_t>(projection_host);
            if (!install_module_view_locked(extension_bag)) {
                set_original_bag_locked(original_bag);
                runtime_state().objects.original_current_direct = static_cast<uint8_t>(original_bag);
                runtime_state().objects.original_current_got = static_cast<uint8_t>(original_bag);
                VIRTBAG_LOG(
                    "extension tab reject reason=projection_host_unavailable source_original_bag=%d target=%d host=%d",
                    original_bag, extension_bag, projection_host);
                return;
            }
            if (virtual_bag::click(&runtime_state().objects.bag, extension_bag) !=
                virtual_bag::ClickResult::kSelected) {
                restore_module_view_locked();
                set_original_bag_locked(original_bag);
                runtime_state().objects.original_current_direct = static_cast<uint8_t>(original_bag);
                runtime_state().objects.original_current_got = static_cast<uint8_t>(original_bag);
                VIRTBAG_LOG(
                    "extension tab reject reason=projection_host_unavailable source_original_bag=%d target=%d host=%d",
                    original_bag, extension_bag, projection_host);
                return;
            }
            persist_state_locked();
            VIRTBAG_LOG("extension tab selected bag=%d original_bag=%d projection_host=%d",
                        extension_bag, original_bag, projection_host);
            return;
        }
        if (virtual_bag::valid_original_transaction_bag(original_bag)) {
            runtime_state().objects.original_current_direct = static_cast<uint8_t>(original_bag);
            runtime_state().objects.original_current_got = static_cast<uint8_t>(original_bag);
            if (virtual_bag::click(&runtime_state().objects.bag, extension_bag) !=
                virtual_bag::ClickResult::kIgnored) {
                install_module_view_locked(extension_bag);
                persist_state_locked();
                VIRTBAG_LOG("extension tab selected bag=%d original_bag=%d", extension_bag,
                            original_bag);
            }
        } else {
            VIRTBAG_LOG("extension tab reject original window reason=invalid_transaction_domain bag=%d",
                        original_bag);
        }
        return;
    }
    if (runtime_state().objects.bag.mode == virtual_bag::Mode::kModule) {
        if (runtime_state().objects.bag.selected != extension_bag) {
            if (virtual_bag::click(&runtime_state().objects.bag, extension_bag) !=
                virtual_bag::ClickResult::kIgnored) {
                install_module_view_locked(extension_bag);
                play_extension_switch_sound();
                VIRTBAG_LOG("extension tab switched bag=%d", extension_bag);
            }
            return;
        }
        // 二次点击 = 原版袋信息面板（原版语义：desc_type=1 + UIEquip_MakeDesc）。
        // MakeDesc 读控件物品（data[0]=袋物品对象）生成详情，SetDescMenu 按
        // desc_type=1 加解除按钮；关闭详情经 UIDesc_SetOff。
        if (virtual_bag::click(&runtime_state().objects.bag, extension_bag) ==
            virtual_bag::ClickResult::kInspected) {
            play_extension_switch_sound();
            void* ctrl = runtime_state().ui.extension_tab_buttons[extension_bag];
            if (ctrl != nullptr && runtime_state().host.game != nullptr &&
                runtime_state().host.write_port->ui.make_item_description != nullptr &&
                runtime_state().host.write_port->ui.set_current_bag != nullptr &&
                runtime_state().host.write_port->ui.set_desc_type != nullptr) {
                // 门禁掩码（P3）：UIEquip_SetDescMenu desc_type=1 分支经 GOT 槽读
                // 当前原版袋索引，0（初始袋）/5（任务袋）短路不生成解除按钮
                // （b8570-b8578）。扩展袋与该全局无关——进入前所在原版袋决定
                // 按钮有无。MakeDesc 同步尾跳 SetDescMenu（b89c0），临时把
                // direct+GOT 双字节掩码为 1 使按钮恒生成，返回后立即恢复快照
                // （成对写约定同 set_original_bag_locked；按钮在 panel+0x68，
                // 不随恢复消失，点击回调已被模块 hook 接管）。Pending 路径在这些
                // UI 回调期间释放运行时锁，并在返回后复核视图身份。
                const int saved_bag = raw_direct_bag_locked();
                const int saved_desc_type = raw_desc_type_locked();
                void* const tab_data = extension_bag_ui_data(ctrl);
                void* const described_item = tab_data != nullptr
                    ? *reinterpret_cast<void**>(tab_data) : nullptr;
                const uint8_t bag_type = runtime_state().objects.bag.types[extension_bag];
                const int bag_capacity = runtime_state().objects.bag.capacities[extension_bag];
                if (tab_data == nullptr || described_item == nullptr) {
                    QOL_LOG_ERROR(QolDomain::kExtensionBag,
                                  "extension bag description rejected missing control item bag=%d control=%p",
                                  extension_bag, ctrl);
                } else if (saved_bag < 0 || saved_desc_type < 0) {
                    QOL_LOG_ERROR(QolDomain::kExtensionBag,
                                  "extension bag description rejected state snapshot bag=%d current_bag=%d desc_type=%d",
                                  extension_bag, saved_bag, saved_desc_type);
                } else if (unlockable_lock == nullptr) {
                    const bool bag_set = qol::contracts::succeeded(
                        runtime_state().host.write_port->ui.set_current_bag(1));
                    const bool desc_set = bag_set && qol::contracts::succeeded(
                        runtime_state().host.write_port->ui.set_desc_type(1));
                    const bool made = desc_set && qol::contracts::succeeded(
                        runtime_state().host.write_port->ui.make_item_description(ctrl));
                    const bool bag_restored = saved_bag < 0 || qol::contracts::succeeded(
                        runtime_state().host.write_port->ui.set_current_bag(saved_bag));
                    const bool desc_restored = saved_desc_type < 0 || qol::contracts::succeeded(
                        runtime_state().host.write_port->ui.set_desc_type(saved_desc_type));
                    if (!bag_set || !desc_set || !made || !bag_restored || !desc_restored) {
                        QOL_LOG_ERROR(QolDomain::kExtensionBag,
                                      "extension bag description UI operation failed bag=%d bag_set=%d desc_set=%d made=%d bag_restored=%d desc_restored=%d",
                                      extension_bag, bag_set ? 1 : 0, desc_set ? 1 : 0,
                                      made ? 1 : 0, bag_restored ? 1 : 0,
                                      desc_restored ? 1 : 0);
                    } else {
                        install_extension_desc_unequip_hook_locked(extension_bag);
                    }
                } else {
                    const uint64_t generation = runtime_state().ui.inventory_generation;
                    void* const root = runtime_state().ui.extension_tab_root;
                    unlockable_lock->unlock();
                    // Original callback operation: g_write_port->ui.make_item_description(ctrl).
                    const bool bag_set = qol::contracts::succeeded(
                        runtime_state().host.write_port->ui.set_current_bag(1));
                    const bool desc_set = bag_set && qol::contracts::succeeded(
                        runtime_state().host.write_port->ui.set_desc_type(1));
                    const bool made = desc_set && qol::contracts::succeeded(
                        runtime_state().host.write_port->ui.make_item_description(ctrl));
                    const bool bag_restored = saved_bag < 0 || qol::contracts::succeeded(
                        runtime_state().host.write_port->ui.set_current_bag(saved_bag));
                    const bool desc_restored = saved_desc_type < 0 || qol::contracts::succeeded(
                        runtime_state().host.write_port->ui.set_desc_type(saved_desc_type));
                    unlockable_lock->lock();
                    bool still_current = runtime_state().enabled.load() &&
                        extension_bag_session_admitted_locked() &&
                        // Former generation predicate: g_inventory_generation == generation.
                        runtime_state().ui.inventory_generation == generation &&
                        runtime_state().ui.extension_tab_root == root &&
                        runtime_state().objects.bag.mode == virtual_bag::Mode::kModule &&
                        runtime_state().objects.bag.selected == extension_bag &&
                        runtime_state().objects.bag.types[extension_bag] == bag_type &&
                        runtime_state().objects.bag.capacities[extension_bag] == bag_capacity &&
                        runtime_state().ui.tab_bag_items[extension_bag] == described_item &&
                        extension_tab_control_is_current_locked(extension_bag, ctrl);
                    if (still_current) {
                        void* const current_data = extension_bag_ui_data(ctrl);
                        still_current = current_data != nullptr &&
                            *reinterpret_cast<void**>(current_data) == described_item;
                    }
                    if (!bag_set || !desc_set || !made || !bag_restored || !desc_restored) {
                        QOL_LOG_ERROR(QolDomain::kExtensionBag,
                                      "extension bag description UI operation failed bag=%d bag_set=%d desc_set=%d made=%d bag_restored=%d desc_restored=%d",
                                      extension_bag, bag_set ? 1 : 0, desc_set ? 1 : 0,
                                      made ? 1 : 0, bag_restored ? 1 : 0,
                                      desc_restored ? 1 : 0);
                    }
                    const bool description_ready = bag_set && desc_set && made &&
                                                   bag_restored && desc_restored;
                    if (!still_current || !description_ready) {
                        if (!still_current) {
                            QOL_LOG_WARN(QolDomain::kExtensionBag,
                                         "extension bag description discarded after view changed bag=%d generation=%llu",
                                         extension_bag,
                                         static_cast<unsigned long long>(generation));
                        }
                        if (made) {
                            bool closed = false;
                            if (runtime_state().host.write_port->ui.description_set_off != nullptr) {
                                unlockable_lock->unlock();
                                closed = qol::contracts::succeeded(
                                    runtime_state().host.write_port->ui.description_set_off());
                                unlockable_lock->lock();
                            }
                            if (!closed) {
                                QOL_LOG_ERROR(QolDomain::kExtensionBag,
                                              "extension bag description close failed bag=%d",
                                              extension_bag);
                            }
                        }
                    } else {
                        install_extension_desc_unequip_hook_locked(extension_bag);
                    }
                }
            } else {
                QOL_LOG_ERROR(QolDomain::kExtensionBag,
                              "extension bag description rejected UI adapter unavailable bag=%d control=%p",
                              extension_bag, ctrl);
            }
            VIRTBAG_LOG("extension bag desc opened bag=%d", extension_bag);
        }
    }
}

void queue_extension_tab_click_locked(int extension_bag) {
    // The enabled gate is the explicit runtime_state().enabled form of g_virtual_bag_enabled.load().
    if (!runtime_state().enabled.load() || !extension_bag_session_admitted_locked() ||
        !virtual_bag::valid_index(extension_bag) ||
        !extension_tab_control_is_current_locked(
            extension_bag, runtime_state().ui.extension_tab_buttons[static_cast<size_t>(extension_bag)]) ||
        runtime_state().ui.extension_tab_generation != runtime_state().ui.inventory_generation) {
        return;
    }
    if (runtime_state().ui.pending_extension_tab == extension_bag &&
        runtime_state().ui.pending_extension_tab_generation == runtime_state().ui.inventory_generation) {
        return;
    }
    runtime_state().ui.pending_extension_tab = extension_bag;
    runtime_state().ui.pending_extension_tab_generation = runtime_state().ui.inventory_generation;
    VIRTBAG_LOG("extension tab pending bag=%d generation=%llu", extension_bag,
                static_cast<unsigned long long>(runtime_state().ui.inventory_generation));
}

void extension_tab_button_clicked(void* ctrl) {
    std::lock_guard<std::mutex> lock(runtime_state().mtx);
    const int extension_bag = extension_tab_index(ctrl);
    queue_extension_tab_click_locked(extension_bag);
}

bool extension_tab_control_is_current_locked(int index, void* control) {
    if (!runtime_state().enabled.load() || !extension_bag_session_admitted_locked() ||
        !virtual_bag::valid_index(index) || control == nullptr ||
        runtime_state().ui.extension_tab_root == nullptr ||
        runtime_state().ui.extension_tab_anchors[index].mounted == 0 ||
        runtime_state().ui.extension_tab_anchors[index].handle != control ||
        runtime_state().ui.extension_tab_buttons[index] != control ||
        runtime_state().ui.extension_tab_generation != runtime_state().ui.inventory_generation ||
        extension_bag_ui_root(qol::contracts::QolUiRootKind::kEquipBagContainer) !=
            runtime_state().ui.extension_tab_root) {
        return false;
    }
    int child_count = 0;
    if (!extension_bag_ui_child_count(runtime_state().ui.extension_tab_root, &child_count)) return false;
    for (int child = 0; child < child_count; ++child) {
        if (extension_bag_ui_child_at(runtime_state().ui.extension_tab_root, child) == control) {
            return valid_child_locked(runtime_state().ui.extension_tab_root, child) == control;
        }
    }
    return false;
}
void commit_pending_extension_tab_locked(std::unique_lock<std::mutex>& lock) {
    if (!virtual_bag::valid_index(runtime_state().ui.pending_extension_tab) ||
        !runtime_state().enabled.load() || !extension_bag_session_admitted_locked() ||
        runtime_state().ui.pending_extension_tab_generation != runtime_state().ui.inventory_generation ||
        runtime_state().ui.extension_tab_generation != runtime_state().ui.inventory_generation ||
        !extension_tab_control_is_current_locked(
            runtime_state().ui.pending_extension_tab,
            runtime_state().ui.extension_tab_buttons[static_cast<size_t>(runtime_state().ui.pending_extension_tab)])) {
        runtime_state().ui.pending_extension_tab = -1;
        runtime_state().ui.pending_extension_tab_generation = 0;
        return;
    }
    const int extension_bag = runtime_state().ui.pending_extension_tab;
    runtime_state().ui.pending_extension_tab = -1;
    runtime_state().ui.pending_extension_tab_generation = 0;
    handle_pending_extension_tab_click_locked(lock, extension_bag);
}

bool extension_c18_native_bag_hit_internal(int64_t x, int64_t y, int* out_native_bag) {
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

bool extension_tab_hit(int index, int64_t x, int64_t y) {
    if (!virtual_bag::valid_index(index)) return false;
    if (runtime_state().objects.bag.capacities[index] == 0) {
        // 未装备袋只有在当前拖动物品满足装备规则时可命中；否则仍保持
        // G-12 的不可命中门禁，避免普通物品误落到未装备标签。
        const int category = drag_source_category_locked();
        if (!is_backpack_category(category)) return false;
    }
    // 标签位置是动态换算的（tab_rel + 袋容器绝对），用控件真实绝对 rect 判断，
    // 不用旧硬编码 kCellX/kCellY（标签左移后二者偏移 36px 致命中失效）。
    void* ctrl = runtime_state().ui.extension_tab_buttons[index];
    if (ctrl == nullptr) return false;
    UiRect rect{};
    return extension_bag_ui_rect(ctrl, &rect) && rect.w > 0 && rect.h > 0 &&
           extension_bag_ui_hit_test(ctrl, x, y, rect);
}
}  // namespace

#undef handle_extension_tab_click_locked
#undef delete_extension_tab_buttons_locked
#undef disable_extension_tab_buttons_locked
#undef install_extension_tab_buttons_locked
#undef extension_tab_buttons_complete_locked
#undef commit_pending_extension_tab_locked
#undef reset_extension_desc_unequip_hook_locked
#undef extension_tab_hit

void handle_extension_tab_click_locked(int extension_bag) {
    handle_extension_tab_click_locked_internal(extension_bag);
}
void extension_bag_ui_play_switch_sound() { play_extension_switch_sound(); }
void extension_bag_ui_handle_tab_click_locked(int extension_bag) {
    handle_extension_tab_click_locked(extension_bag);
}

bool extension_tab_hit(int index, int64_t x, int64_t y) {
    return extension_tab_hit_internal(index, x, y);
}
bool extension_c18_native_bag_hit(int64_t x, int64_t y, int* out_native_bag) {
    return extension_c18_native_bag_hit_internal(x, y, out_native_bag);
}
bool extension_bag_runtime_is_backpack_category(int category) {
    return is_backpack_category(category);
}
void delete_extension_tab_buttons_locked() {
    delete_extension_tab_buttons_locked_internal();
}
void disable_extension_tab_buttons_locked() {
    disable_extension_tab_buttons_locked_internal();
}
void install_extension_tab_buttons_locked() {
    install_extension_tab_buttons_locked_internal();
}
void prepare_c18_native_bag_layout_locked() {
    prepare_c18_native_bag_layout_locked_impl();
}
bool extension_tab_buttons_complete_locked() {
    return extension_tab_buttons_complete_locked_internal();
}
void reset_extension_desc_unequip_hook_locked() {
    reset_extension_desc_unequip_hook_locked_internal();
}
void commit_pending_extension_tab_locked(std::unique_lock<std::mutex>& lock) {
    commit_pending_extension_tab_locked_internal(lock);
}
